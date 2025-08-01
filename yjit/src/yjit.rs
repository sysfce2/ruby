use crate::codegen::*;
use crate::core::*;
use crate::cruby::*;
use crate::invariants::*;
use crate::options::*;
use crate::stats::YjitExitLocations;
use crate::stats::incr_counter;
use crate::stats::with_compile_time;

use std::os::raw::{c_char, c_int};
use std::time::Instant;
use crate::log::Log;

/// Is YJIT on? The interpreter uses this variable to decide whether to trigger
/// compilation. See jit_exec() and jit_compile().
#[allow(non_upper_case_globals)]
#[no_mangle]
pub static mut rb_yjit_enabled_p: bool = false;

// Time when YJIT was yjit was initialized (see yjit_init)
pub static mut YJIT_INIT_TIME: Option<Instant> = None;

/// Parse one command-line option.
/// This is called from ruby.c
#[no_mangle]
pub extern "C" fn rb_yjit_parse_option(str_ptr: *const c_char) -> bool {
    return parse_option(str_ptr).is_some();
}

#[no_mangle]
pub extern "C" fn rb_yjit_option_disable() -> bool {
    return get_option!(disable);
}

/// Like rb_yjit_enabled_p, but for Rust code.
pub fn yjit_enabled_p() -> bool {
    unsafe { rb_yjit_enabled_p }
}

/// This function is called from C code
#[no_mangle]
pub extern "C" fn rb_yjit_init(yjit_enabled: bool) {
    // Register the method codegen functions. This must be done at boot.
    yjit_reg_method_codegen_fns();

    // If --yjit-disable, yjit_init() will not be called until RubyVM::YJIT.enable.
    if yjit_enabled {
        yjit_init();
    }
}

/// Initialize and enable YJIT. You should call this at boot or with GVL.
fn yjit_init() {
    // TODO: need to make sure that command-line options have been
    // initialized by CRuby

    // Call YJIT hooks before enabling YJIT to avoid compiling the hooks themselves
    unsafe {
        let yjit = rb_const_get(rb_cRubyVM, rust_str_to_id("YJIT"));
        rb_funcall(yjit, rust_str_to_id("call_jit_hooks"), 0);
    }

    // Catch panics to avoid UB for unwinding into C frames.
    // See https://doc.rust-lang.org/nomicon/exception-safety.html
    let result = std::panic::catch_unwind(|| {
        Invariants::init();
        CodegenGlobals::init();
        YjitExitLocations::init();
        ids::init();

        rb_bug_panic_hook();

        // YJIT enabled and initialized successfully
        assert!(unsafe{ !rb_yjit_enabled_p });
        unsafe { rb_yjit_enabled_p = true; }
    });

    if let Err(_) = result {
        println!("YJIT: yjit_init() panicked. Aborting.");
        std::process::abort();
    }

    // Make sure --yjit-perf doesn't append symbols to an old file
    if get_option!(perf_map).is_some() {
        let perf_map = format!("/tmp/perf-{}.map", std::process::id());
        let _ = std::fs::remove_file(&perf_map);
        println!("YJIT perf map: {perf_map}");
    }

    // Note the time when YJIT was initialized
    unsafe {
        YJIT_INIT_TIME = Some(Instant::now());
    }
}

#[no_mangle]
pub extern "C" fn rb_yjit_free_at_exit() {
    yjit_shutdown_free_codegen_table();
}

/// At the moment, we abort in all cases we panic.
/// To aid with getting diagnostics in the wild without requiring
/// people to set RUST_BACKTRACE=1, register a panic hook that crash using rb_bug().
/// rb_bug() might not be as good at printing a call trace as Rust's stdlib, but
/// it dumps some other info that might be relevant.
///
/// In case we want to start doing fancier exception handling with panic=unwind,
/// we can revisit this later. For now, this helps to get us good bug reports.
fn rb_bug_panic_hook() {
    use std::env;
    use std::panic;
    use std::io::{stderr, Write};

    // Probably the default hook. We do this very early during process boot.
    let previous_hook = panic::take_hook();

    panic::set_hook(Box::new(move |panic_info| {
        // Not using `eprintln` to avoid double panic.
        let _ = stderr().write_all(b"ruby: YJIT has panicked. More info to follow...\n");

        // Always show a Rust backtrace.
        env::set_var("RUST_BACKTRACE", "1");
        previous_hook(panic_info);

        // Abort with rb_bug(). It has a length limit on the message.
        let panic_message = &format!("{}", panic_info)[..];
        let len = std::cmp::min(0x100, panic_message.len()) as c_int;
        unsafe { rb_bug(b"YJIT: %*s\0".as_ref().as_ptr() as *const c_char, len, panic_message.as_ptr()); }
    }));
}

/// Called from C code to begin compiling a function
/// NOTE: this should be wrapped in RB_VM_LOCK_ENTER(), rb_vm_barrier() on the C side
/// If jit_exception is true, compile JIT code for handling exceptions.
/// See jit_compile_exception() for details.
#[no_mangle]
pub extern "C" fn rb_yjit_iseq_gen_entry_point(iseq: IseqPtr, ec: EcPtr, jit_exception: bool) -> *const u8 {
    // Don't compile when there is insufficient native stack space
    if unsafe { rb_ec_stack_check(ec as _) } != 0 {
        return std::ptr::null();
    }

    // Reject ISEQs with very large temp stacks,
    // this will allow us to use u8/i8 values to track stack_size and sp_offset
    let stack_max = unsafe { rb_get_iseq_body_stack_max(iseq) };
    if stack_max >= i8::MAX as u32 {
        incr_counter!(iseq_stack_too_large);
        return std::ptr::null();
    }

    // Reject ISEQs that are too long,
    // this will allow us to use u16 for instruction indices if we want to,
    // very long ISEQs are also much more likely to be initialization code
    let iseq_size = unsafe { get_iseq_encoded_size(iseq) };
    if iseq_size >= u16::MAX as u32 {
        incr_counter!(iseq_too_long);
        return std::ptr::null();
    }

    // If a custom call threshold was not specified on the command-line and
    // this is a large application (has very many ISEQs), switch to
    // using the call threshold for large applications after this entry point
    use crate::stats::rb_yjit_live_iseq_count;
    if unsafe { rb_yjit_call_threshold } == SMALL_CALL_THRESHOLD && unsafe { rb_yjit_live_iseq_count } > LARGE_ISEQ_COUNT {
        unsafe { rb_yjit_call_threshold = LARGE_CALL_THRESHOLD; };
    }

    let maybe_code_ptr = with_compile_time(|| { gen_entry_point(iseq, ec, jit_exception) });

    match maybe_code_ptr {
        Some(ptr) => ptr,
        None => std::ptr::null(),
    }
}

/// Free and recompile all existing JIT code
#[no_mangle]
pub extern "C" fn rb_yjit_code_gc(_ec: EcPtr, _ruby_self: VALUE) -> VALUE {
    if !yjit_enabled_p() {
        return Qnil;
    }

    with_vm_lock(src_loc!(), || {
        let cb = CodegenGlobals::get_inline_cb();
        let ocb = CodegenGlobals::get_outlined_cb();
        cb.code_gc(ocb);
    });

    Qnil
}

/// Enable YJIT compilation, returning true if YJIT was previously disabled
#[no_mangle]
pub extern "C" fn rb_yjit_enable(_ec: EcPtr, _ruby_self: VALUE, gen_stats: VALUE, print_stats: VALUE, gen_log: VALUE, print_log: VALUE, mem_size: VALUE, call_threshold: VALUE) -> VALUE {
    with_vm_lock(src_loc!(), || {

        if !mem_size.nil_p() {
            let mem_size_mb = mem_size.as_isize() >> 1;
            let mem_size_bytes = mem_size_mb * 1024 * 1024;
            unsafe {
                OPTIONS.mem_size = mem_size_bytes as usize;
            }
        }

        if !call_threshold.nil_p() {
            let threshold = call_threshold.as_isize() >> 1;
            unsafe {
                rb_yjit_call_threshold = threshold as u64;
            }
        }

        // Initialize and enable YJIT
        if gen_stats.test() {
            unsafe {
                OPTIONS.gen_stats = gen_stats.test();
                OPTIONS.print_stats = print_stats.test();
            }
        }

        if gen_log.test() {
            unsafe {
                if print_log.test() {
                    OPTIONS.log = Some(LogOutput::Stderr);
                } else {
                    OPTIONS.log = Some(LogOutput::MemoryOnly);
                }

                Log::init();
            }
        }

        yjit_init();

        // Add "+YJIT" to RUBY_DESCRIPTION
        extern "C" {
            fn ruby_set_yjit_description();
        }
        unsafe { ruby_set_yjit_description(); }

        Qtrue
    })
}

/// Simulate a situation where we are out of executable memory
#[no_mangle]
pub extern "C" fn rb_yjit_simulate_oom_bang(_ec: EcPtr, _ruby_self: VALUE) -> VALUE {
    // If YJIT is not enabled, do nothing
    if !yjit_enabled_p() {
        return Qnil;
    }

    // Enabled in debug mode only for security
    if cfg!(debug_assertions) {
        let cb = CodegenGlobals::get_inline_cb();
        let ocb = CodegenGlobals::get_outlined_cb().unwrap();
        cb.set_pos(cb.get_mem_size());
        ocb.set_pos(ocb.get_mem_size());
    }

    return Qnil;
}

/// Push a C method frame if the given PC is supposed to lazily push one.
/// This is called from rb_raise() (at rb_exc_new_str()) and other functions
/// that may make a method call (e.g. rb_to_int()).
#[no_mangle]
pub extern "C" fn rb_yjit_lazy_push_frame(pc: *mut VALUE) {
    if !yjit_enabled_p() {
        return;
    }

    incr_counter!(num_lazy_frame_check);
    if let Some(&(cme, recv_idx)) = CodegenGlobals::get_pc_to_cfunc().get(&pc) {
        incr_counter!(num_lazy_frame_push);
        unsafe { rb_vm_push_cfunc_frame(cme, recv_idx as i32) }
    }
}
