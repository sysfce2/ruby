// We use the YARV bytecode constants which have a CRuby-style name
#![allow(non_upper_case_globals)]

use crate::{
    cruby::*,
    get_option,
    options::DumpHIR, profile::{get_or_create_iseq_payload, InsnProfile}
};
use std::collections::{HashMap, HashSet};

#[derive(Copy, Clone, Eq, PartialEq, Hash, Debug)]
pub struct InsnId(pub usize);

impl Into<usize> for InsnId {
    fn into(self) -> usize {
        self.0
    }
}

impl std::fmt::Display for InsnId {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "v{}", self.0)
    }
}

#[derive(Copy, Clone, Eq, PartialEq, Hash, Debug)]
pub struct BlockId(usize);

impl std::fmt::Display for BlockId {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "bb{}", self.0)
    }
}

fn write_vec<T: std::fmt::Display>(f: &mut std::fmt::Formatter, objs: &Vec<T>) -> std::fmt::Result {
    write!(f, "[")?;
    let mut prefix = "";
    for obj in objs {
        write!(f, "{prefix}{obj}")?;
        prefix = ", ";
    }
    write!(f, "]")
}

impl std::fmt::Display for VALUE {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            val if val.fixnum_p() => write!(f, "Fixnum({})", val.as_fixnum()),
            &Qnil => write!(f, "nil"),
            &Qtrue => write!(f, "true"),
            &Qfalse => write!(f, "false"),
            val => write!(f, "VALUE({:#X?})", val.as_ptr::<u8>()),
        }
    }
}

#[derive(Debug, PartialEq, Clone)]
pub struct BranchEdge {
    target: BlockId,
    args: Vec<InsnId>,
}

impl std::fmt::Display for BranchEdge {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "{}(", self.target)?;
        let mut prefix = "";
        for arg in &self.args {
            write!(f, "{prefix}{arg}")?;
            prefix = ", ";
        }
        write!(f, ")")
    }
}

#[derive(Debug, PartialEq, Clone)]
pub struct CallInfo {
    name: String,
}

/// Invalidation reasons
#[derive(Debug, Clone)]
pub enum Invariant {
    /// Basic operation is redefined
    BOPRedefined {
        /// {klass}_REDEFINED_OP_FLAG
        klass: RedefinitionFlag,
        /// BOP_{bop}
        bop: ruby_basic_operators,
    },
}

#[derive(Debug, Clone)]
pub enum Insn {
    PutSelf,
    // TODO(max): We probably want to make this an enum so we are not limited to Ruby heap objects
    Const { val: VALUE },
    // SSA block parameter. Also used for function parameters in the function's entry block.
    Param { idx: usize },

    StringCopy { val: InsnId },
    StringIntern { val: InsnId },

    NewArray { count: usize },
    ArraySet { idx: usize, val: InsnId },
    ArrayDup { val: InsnId },

    // Check if the value is truthy and "return" a C boolean. In reality, we will likely fuse this
    // with IfTrue/IfFalse in the backend to generate jcc.
    Test { val: InsnId },
    Defined { op_type: usize, obj: VALUE, pushval: VALUE, v: InsnId },
    GetConstantPath { ic: *const u8 },

    //NewObject?
    //SetIvar {},
    //GetIvar {},

    // Own a FrameState so that instructions can look up their dominating FrameState when
    // generating deopt side-exits and frame reconstruction metadata. Does not directly generate
    // any code.
    Snapshot { state: FrameState },

    // Unconditional jump
    Jump(BranchEdge),

    // Conditional branch instructions
    IfTrue { val: InsnId, target: BranchEdge },
    IfFalse { val: InsnId, target: BranchEdge },

    // Call a C function
    // NOTE: should we store the C function name for pretty-printing?
    //       or can we backtranslate the function pointer into a name string?
    CCall { cfun: *const u8, args: Vec<InsnId> },

    // Send with dynamic dispatch
    // Ignoring keyword arguments etc for now
    Send { self_val: InsnId, call_info: CallInfo, args: Vec<InsnId> },

    // Control flow instructions
    Return { val: InsnId },

    /// Fixnum + Fixnum
    FixnumAdd { recv: InsnId, obj: InsnId },

    /// Side-exist if val doesn't have the expected type.
    // TODO: Replace is_fixnum with the type lattice
    GuardType { val: InsnId, is_fixnum: bool },

    /// Generate no code (or padding if necessary) and insert a patch point
    /// that can be rewritten to a side exit when the Invariant is broken.
    PatchPoint(Invariant),
}

#[derive(Default, Debug)]
pub struct Block {
    params: Vec<InsnId>,
    insns: Vec<InsnId>,
}

impl Block {
}

struct FunctionPrinter<'a> {
    fun: &'a Function,
    display_snapshot: bool,
}

impl<'a> FunctionPrinter<'a> {
    fn without_snapshot(fun: &'a Function) -> FunctionPrinter<'a> {
        FunctionPrinter { fun, display_snapshot: false }
    }

    fn with_snapshot(fun: &'a Function) -> FunctionPrinter<'a> {
        FunctionPrinter { fun, display_snapshot: true }
    }
}

/// Union-Find (Disjoint-Set) is a data structure for managing disjoint sets that has an interface
/// of two operations:
///
/// * find (what set is this item part of?)
/// * union (join these two sets)
///
/// Union-Find identifies sets by their *representative*, which is some chosen element of the set.
/// This is implemented by structuring each set as its own graph component with the representative
/// pointing at nothing. For example:
///
/// * A -> B -> C
/// * D -> E
///
/// This represents two sets `C` and `E`, with three and two members, respectively. In this
/// example, `find(A)=C`, `find(C)=C`, `find(D)=E`, and so on.
///
/// To union sets, call `make_equal_to` on any set element. That is, `make_equal_to(A, D)` and
/// `make_equal_to(B, E)` have the same result: the two sets are joined into the same graph
/// component. After this operation, calling `find` on any element will return `E`.
///
/// This is a useful data structure in compilers because it allows in-place rewriting without
/// linking/unlinking instructions and without replacing all uses. When calling `make_equal_to` on
/// any instruction, all of its uses now implicitly point to the replacement.
///
/// This does mean that pattern matching and analysis of the instruction graph must be careful to
/// call `find` whenever it is inspecting an instruction (or its operands). If not, this may result
/// in missing optimizations.
#[derive(Debug)]
struct UnionFind<T: Copy + Into<usize>> {
    forwarded: Vec<Option<T>>,
}

impl<T: Copy + Into<usize> + PartialEq> UnionFind<T> {
    fn new() -> UnionFind<T> {
        UnionFind { forwarded: vec![] }
    }

    /// Private. Return the internal representation of the forwarding pointer for a given element.
    fn at(&self, idx: T) -> Option<T> {
        self.forwarded.get(idx.into()).map(|x| *x).flatten()
    }

    /// Private. Set the internal representation of the forwarding pointer for the given element
    /// `idx`. Extend the internal vector if necessary.
    fn set(&mut self, idx: T, value: T) {
        if idx.into() >= self.forwarded.len() {
            self.forwarded.resize(idx.into()+1, None);
        }
        self.forwarded[idx.into()] = Some(value);
    }

    /// Find the set representative for `insn`. Perform path compression at the same time to speed
    /// up further find operations. For example, before:
    ///
    /// `A -> B -> C`
    ///
    /// and after `find(A)`:
    ///
    /// ```
    /// A -> C
    /// B ---^
    /// ```
    pub fn find(&mut self, insn: T) -> T {
        let result = self.find_const(insn);
        if result != insn {
            // Path compression
            self.set(insn, result);
        }
        result
    }

    /// Find the set representative for `insn` without doing path compression.
    pub fn find_const(&self, insn: T) -> T {
        let mut result = insn;
        loop {
            match self.at(result) {
                None => return result,
                Some(insn) => result = insn,
            }
        }
    }

    /// Union the two sets containing `insn` and `target` such that every element in `insn`s set is
    /// now part of `target`'s. Neither argument must be the representative in its set.
    pub fn make_equal_to(&mut self, insn: T, target: T) {
        let found = self.find(insn);
        self.set(found, target);
    }
}

#[derive(Debug)]
pub struct Function {
    // ISEQ this function refers to
    iseq: *const rb_iseq_t,

    // TODO: get method name and source location from the ISEQ

    pub insns: Vec<Insn>,
    union_find: UnionFind<InsnId>,
    blocks: Vec<Block>,
    entry_block: BlockId,
}

impl Function {
    fn new(iseq: *const rb_iseq_t) -> Function {
        Function {
            iseq,
            insns: vec![],
            union_find: UnionFind::new(),
            blocks: vec![Block::default()],
            entry_block: BlockId(0)
        }
    }

    // Add an instruction to an SSA block
    fn push_insn(&mut self, block: BlockId, insn: Insn) -> InsnId {
        let id = InsnId(self.insns.len());
        self.insns.push(insn);
        self.blocks[block.0].insns.push(id);
        id
    }

    fn new_block(&mut self) -> BlockId {
        let id = BlockId(self.blocks.len());
        self.blocks.push(Block::default());
        id
    }

    /// Use for pattern matching over instructions in a union-find-safe way. For example:
    /// ```rust
    /// match func.find(insn_id) {
    ///   IfTrue { val, target } if func.is_truthy(val) => {
    ///     func.make_equal_to(insn_id, block, Insn::Jump(target));
    ///   }
    ///   _ => {}
    /// }
    /// ```
    fn find(&mut self, insn_id: InsnId) -> Insn {
        let insn_id = self.union_find.find(insn_id);
        use Insn::*;
        match &self.insns[insn_id.0] {
            result@(PutSelf | Const {..} | Param {..} | NewArray {..} | GetConstantPath {..}) => result.clone(),
            StringCopy { val } => StringCopy { val: self.union_find.find(*val) },
            StringIntern { val } => StringIntern { val: self.union_find.find(*val) },
            Test { val } => Test { val: self.union_find.find(*val) },
            insn => todo!("find({insn:?})"),
        }
    }

    /// Replace `insn` with the new instruction `replacement`, which will get appended to `insns`.
    fn make_equal_to(&mut self, insn: InsnId, block: BlockId, replacement: Insn) {
        let new_insn = self.push_insn(block, replacement);
        self.union_find.make_equal_to(insn, new_insn);
    }
}

impl<'a> std::fmt::Display for FunctionPrinter<'a> {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        let fun = &self.fun;
        for (block_id, block) in fun.blocks.iter().enumerate() {
            let block_id = BlockId(block_id);
            writeln!(f, "{block_id}:")?;
            for insn_id in &block.insns {
                if !self.display_snapshot && matches!(fun.insns[insn_id.0], Insn::Snapshot {..}) {
                    continue;
                }
                write!(f, "  {insn_id} = ")?;
                match &fun.insns[insn_id.0] {
                    Insn::Param { idx } => { write!(f, "Param {idx}")?; }
                    Insn::Const { val } => { write!(f, "Const {val}")?; }
                    Insn::IfTrue { val, target } => { write!(f, "IfTrue {val}, {target}")?; }
                    Insn::IfFalse { val, target } => { write!(f, "IfFalse {val}, {target}")?; }
                    Insn::Jump(target) => { write!(f, "Jump {target}")?; }
                    Insn::Return { val } => { write!(f, "Return {val}")?; }
                    Insn::NewArray { count } => { write!(f, "NewArray {count}")?; }
                    Insn::ArraySet { idx, val } => { write!(f, "ArraySet {idx}, {val}")?; }
                    Insn::ArrayDup { val } => { write!(f, "ArrayDup {val}")?; }
                    Insn::Send { self_val, call_info, args } => {
                        write!(f, "Send {self_val}, :{}", call_info.name)?;
                        for arg in args {
                            write!(f, ", {arg}")?;
                        }
                    }
                    Insn::Test { val } => { write!(f, "Test {val}")?; }
                    Insn::Snapshot { state } => { write!(f, "Snapshot {state}")?; }
                    insn => { write!(f, "{insn:?}")?; }
                }
                writeln!(f, "")?;
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone)]
pub struct FrameState {
    iseq: IseqPtr,
    // Ruby bytecode instruction pointer
    pc: VALUE,

    stack: Vec<InsnId>,
    locals: Vec<InsnId>,
}

/// Compute the index of a local variable from its slot index
fn ep_offset_to_local_idx(iseq: IseqPtr, ep_offset: u32) -> usize {
    // Layout illustration
    // This is an array of VALUE
    //                                           | VM_ENV_DATA_SIZE |
    //                                           v                  v
    // low addr <+-------+-------+-------+-------+------------------+
    //           |local 0|local 1|  ...  |local n|       ....       |
    //           +-------+-------+-------+-------+------------------+
    //           ^       ^                       ^                  ^
    //           +-------+---local_table_size----+         cfp->ep--+
    //                   |                                          |
    //                   +------------------ep_offset---------------+
    //
    // See usages of local_var_name() from iseq.c for similar calculation.

    // Equivalent of iseq->body->local_table_size
    let local_table_size: i32 = unsafe { get_iseq_body_local_table_size(iseq) }
        .try_into()
        .unwrap();
    let op = (ep_offset - VM_ENV_DATA_SIZE) as i32;
    let local_idx = local_table_size - op - 1;
    assert!(local_idx >= 0 && local_idx < local_table_size);
    local_idx.try_into().unwrap()
}

impl FrameState {
    fn new(iseq: IseqPtr) -> FrameState {
        FrameState { iseq, pc: VALUE(0), stack: vec![], locals: vec![] }
    }

    fn push(&mut self, opnd: InsnId) {
        self.stack.push(opnd);
    }

    fn top(&self) -> Result<InsnId, ParseError> {
        self.stack.last().ok_or_else(|| ParseError::StackUnderflow(self.clone())).copied()
    }

    fn pop(&mut self) -> Result<InsnId, ParseError> {
        self.stack.pop().ok_or_else(|| ParseError::StackUnderflow(self.clone()))
    }

    fn setn(&mut self, n: usize, opnd: InsnId) {
        let idx = self.stack.len() - n - 1;
        self.stack[idx] = opnd;
    }

    fn setlocal(&mut self, ep_offset: u32, opnd: InsnId) {
        let idx = ep_offset_to_local_idx(self.iseq, ep_offset);
        self.locals[idx] = opnd;
    }

    fn getlocal(&mut self, ep_offset: u32) -> InsnId {
        let idx = ep_offset_to_local_idx(self.iseq, ep_offset);
        self.locals[idx]
    }

    fn as_args(&self) -> Vec<InsnId> {
        self.locals.iter().chain(self.stack.iter()).map(|op| op.clone()).collect()
    }
}

impl std::fmt::Display for FrameState {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "FrameState {{ pc: {:?}, stack: ", self.pc.as_ptr::<u8>())?;
        write_vec(f, &self.stack)?;
        write!(f, ", locals: ")?;
        write_vec(f, &self.locals)?;
        write!(f, " }}")
    }
}

/// Get YARV instruction argument
fn get_arg(pc: *const VALUE, arg_idx: isize) -> VALUE {
    unsafe { *(pc.offset(arg_idx + 1)) }
}

/// Compute YARV instruction index at relative offset
fn insn_idx_at_offset(idx: u32, offset: i64) -> u32 {
    ((idx as isize) + (offset as isize)) as u32
}

fn compute_jump_targets(iseq: *const rb_iseq_t) -> Vec<u32> {
    let iseq_size = unsafe { get_iseq_encoded_size(iseq) };
    let mut insn_idx = 0;
    let mut jump_targets = HashSet::new();
    while insn_idx < iseq_size {
        // Get the current pc and opcode
        let pc = unsafe { rb_iseq_pc_at_idx(iseq, insn_idx.into()) };

        // try_into() call below is unfortunate. Maybe pick i32 instead of usize for opcodes.
        let opcode: u32 = unsafe { rb_iseq_opcode_at_pc(iseq, pc) }
            .try_into()
            .unwrap();
        insn_idx += insn_len(opcode as usize);
        match opcode {
            YARVINSN_branchunless | YARVINSN_jump | YARVINSN_branchif | YARVINSN_branchnil => {
                let offset = get_arg(pc, 0).as_i64();
                jump_targets.insert(insn_idx_at_offset(insn_idx, offset));
            }
            YARVINSN_leave | YARVINSN_opt_invokebuiltin_delegate_leave => {
                if insn_idx < iseq_size {
                    jump_targets.insert(insn_idx);
                }
            }
            _ => {}
        }
    }
    let mut result = jump_targets.into_iter().collect::<Vec<_>>();
    result.sort();
    result
}

#[derive(Debug)]
pub enum ParseError {
    StackUnderflow(FrameState),
    UnknownOpcode(String),
}

fn num_lead_params(iseq: *const rb_iseq_t) -> usize {
    let result = unsafe { rb_get_iseq_body_param_lead_num(iseq) };
    assert!(result >= 0, "Can't have negative # of parameters");
    result as usize
}

/// Return the number of locals in the current ISEQ (includes parameters)
fn num_locals(iseq: *const rb_iseq_t) -> usize {
    (unsafe { get_iseq_body_local_table_size(iseq) }) as usize
}

/// Compile ISEQ into High-level IR
pub fn iseq_to_hir(iseq: *const rb_iseq_t) -> Result<Function, ParseError> {
    let mut fun = Function::new(iseq);
    // Compute a map of PC->Block by finding jump targets
    let jump_targets = compute_jump_targets(iseq);
    let mut insn_idx_to_block = HashMap::new();
    for insn_idx in jump_targets {
        if insn_idx == 0 {
            todo!("Separate entry block for param/self/...");
        }
        insn_idx_to_block.insert(insn_idx, fun.new_block());
    }

    // Iteratively fill out basic blocks using a queue
    // TODO(max): Basic block arguments at edges
    let mut queue = std::collections::VecDeque::new();
    let mut entry_state = FrameState::new(iseq);
    for idx in 0..num_locals(iseq) {
        if idx < num_lead_params(iseq) {
            entry_state.locals.push(fun.push_insn(fun.entry_block, Insn::Param { idx }));
        } else {
            entry_state.locals.push(fun.push_insn(fun.entry_block, Insn::Const { val: Qnil }));
        }
    }
    queue.push_back((entry_state, fun.entry_block, /*insn_idx=*/0 as u32));

    let mut visited = HashSet::new();

    let iseq_size = unsafe { get_iseq_encoded_size(iseq) };
    let payload = get_or_create_iseq_payload(iseq);
    while let Some((incoming_state, block, mut insn_idx)) = queue.pop_front() {
        if visited.contains(&block) { continue; }
        visited.insert(block);
        let mut state = if insn_idx == 0 { incoming_state.clone() } else {
            let mut result = FrameState::new(iseq);
            let mut idx = 0;
            for _ in 0..incoming_state.locals.len() {
                result.locals.push(fun.push_insn(block, Insn::Param { idx }));
                idx += 1;
            }
            for _ in incoming_state.stack {
                result.stack.push(fun.push_insn(block, Insn::Param { idx }));
                idx += 1;
            }
            result
        };
        while insn_idx < iseq_size {
            // Get the current pc and opcode
            let pc = unsafe { rb_iseq_pc_at_idx(iseq, insn_idx.into()) };
            state.pc = unsafe { *pc };
            fun.push_insn(block, Insn::Snapshot { state: state.clone() });

            // try_into() call below is unfortunate. Maybe pick i32 instead of usize for opcodes.
            let opcode: u32 = unsafe { rb_iseq_opcode_at_pc(iseq, pc) }
            .try_into()
                .unwrap();
            // Move to the next instruction to compile
            insn_idx += insn_len(opcode as usize);

            match opcode {
                YARVINSN_nop => {},
                YARVINSN_putnil => { state.push(fun.push_insn(block, Insn::Const { val: Qnil })); },
                YARVINSN_putobject => { state.push(fun.push_insn(block, Insn::Const { val: get_arg(pc, 0) })); },
                YARVINSN_putstring | YARVINSN_putchilledstring => {
                    // TODO(max): Do something different for chilled string
                    let val = fun.push_insn(block, Insn::Const { val: get_arg(pc, 0) });
                    let insn_id = fun.push_insn(block, Insn::StringCopy { val });
                    state.push(insn_id);
                }
                YARVINSN_putself => { state.push(fun.push_insn(block, Insn::PutSelf)); }
                YARVINSN_intern => {
                    let val = state.pop()?;
                    let insn_id = fun.push_insn(block, Insn::StringIntern { val });
                    state.push(insn_id);
                }
                YARVINSN_newarray => {
                    let count = get_arg(pc, 0).as_usize();
                    let insn_id = fun.push_insn(block, Insn::NewArray { count });
                    for idx in (0..count).rev() {
                        fun.push_insn(block, Insn::ArraySet { idx, val: state.pop()? });
                    }
                    state.push(insn_id);
                }
                YARVINSN_duparray => {
                    let val = fun.push_insn(block, Insn::Const { val: get_arg(pc, 0) });
                    let insn_id = fun.push_insn(block, Insn::ArrayDup { val });
                    state.push(insn_id);
                }
                YARVINSN_putobject_INT2FIX_0_ => {
                    state.push(fun.push_insn(block, Insn::Const { val: VALUE::fixnum_from_usize(0) }));
                }
                YARVINSN_putobject_INT2FIX_1_ => {
                    state.push(fun.push_insn(block, Insn::Const { val: VALUE::fixnum_from_usize(1) }));
                }
                YARVINSN_defined => {
                    let op_type = get_arg(pc, 0).as_usize();
                    let obj = get_arg(pc, 0);
                    let pushval = get_arg(pc, 0);
                    let v = state.pop()?;
                    state.push(fun.push_insn(block, Insn::Defined { op_type, obj, pushval, v }));
                }
                YARVINSN_opt_getconstant_path => {
                    let ic = get_arg(pc, 0).as_ptr::<u8>();
                    state.push(fun.push_insn(block, Insn::GetConstantPath { ic }));
                }
                YARVINSN_branchunless => {
                    let offset = get_arg(pc, 0).as_i64();
                    let val = state.pop()?;
                    let test_id = fun.push_insn(block, Insn::Test { val });
                    // TODO(max): Check interrupts
                    let target_idx = insn_idx_at_offset(insn_idx, offset);
                    let target = insn_idx_to_block[&target_idx];
                    // TODO(max): Merge locals/stack for bb arguments
                    let _branch_id = fun.push_insn(block, Insn::IfFalse {
                        val: test_id,
                        target: BranchEdge { target, args: state.as_args() }
                    });
                    queue.push_back((state.clone(), target, target_idx));
                }
                YARVINSN_branchif => {
                    let offset = get_arg(pc, 0).as_i64();
                    let val = state.pop()?;
                    let test_id = fun.push_insn(block, Insn::Test { val });
                    // TODO(max): Check interrupts
                    let target_idx = insn_idx_at_offset(insn_idx, offset);
                    let target = insn_idx_to_block[&target_idx];
                    // TODO(max): Merge locals/stack for bb arguments
                    let _branch_id = fun.push_insn(block, Insn::IfTrue {
                        val: test_id,
                        target: BranchEdge { target, args: state.as_args() }
                    });
                    queue.push_back((state.clone(), target, target_idx));
                }
                YARVINSN_jump => {
                    let offset = get_arg(pc, 0).as_i64();
                    // TODO(max): Check interrupts
                    let target_idx = insn_idx_at_offset(insn_idx, offset);
                    let target = insn_idx_to_block[&target_idx];
                    let _branch_id = fun.push_insn(block, Insn::Jump(
                        BranchEdge { target, args: state.as_args() }
                    ));
                    queue.push_back((state.clone(), target, target_idx));
                    break;  // Don't enqueue the next block as a successor
                }
                YARVINSN_opt_nil_p => {
                    let recv = state.pop()?;
                    state.push(fun.push_insn(block, Insn::Send { self_val: recv, call_info: CallInfo { name: "nil?".into() }, args: vec![] }));
                }
                YARVINSN_getlocal_WC_0 => {
                    let ep_offset = get_arg(pc, 0).as_u32();
                    let val = state.getlocal(ep_offset);
                    state.push(val);
                }
                YARVINSN_setlocal_WC_0 => {
                    let ep_offset = get_arg(pc, 0).as_u32();
                    let val = state.pop()?;
                    state.setlocal(ep_offset, val);
                }
                YARVINSN_pop => { state.pop()?; }
                YARVINSN_dup => { state.push(state.top()?); }
                YARVINSN_swap => {
                    let right = state.pop()?;
                    let left = state.pop()?;
                    state.push(right);
                    state.push(left);
                }
                YARVINSN_setn => {
                    let n = get_arg(pc, 0).as_usize();
                    let top = state.top()?;
                    state.setn(n, top);
                }

                YARVINSN_opt_plus | YARVINSN_zjit_opt_plus => {
                    let right = state.pop()?;
                    let left = state.pop()?;
                    if let Some(InsnProfile::OptPlus { recv_is_fixnum: true, obj_is_fixnum: true }) = payload.get_insn_profile(insn_idx as usize) {
                        state.push(fun.push_insn(block, Insn::PatchPoint(Invariant::BOPRedefined { klass: INTEGER_REDEFINED_OP_FLAG, bop: BOP_PLUS })));
                        let left_fixnum = fun.push_insn(block, Insn::GuardType { val: left, is_fixnum: true });
                        state.push(left_fixnum);
                        let right_fixnum = fun.push_insn(block, Insn::GuardType { val: right, is_fixnum: true });
                        state.push(right_fixnum);
                        state.push(fun.push_insn(block, Insn::FixnumAdd { recv: left_fixnum, obj: right_fixnum }));
                    } else {
                        state.push(fun.push_insn(block, Insn::Send { self_val: left, call_info: CallInfo { name: "+".into() }, args: vec![right] }));
                    }
                }
                YARVINSN_opt_div => {
                    let right = state.pop()?;
                    let left = state.pop()?;
                    state.push(fun.push_insn(block, Insn::Send { self_val: left, call_info: CallInfo { name: "/".into() }, args: vec![right] }));
                }

                YARVINSN_opt_lt => {
                    let right = state.pop()?;
                    let left = state.pop()?;
                    state.push(fun.push_insn(block, Insn::Send { self_val: left, call_info: CallInfo { name: "<".into() }, args: vec![right] }));
                }
                YARVINSN_opt_ltlt => {
                    let right = state.pop()?;
                    let left = state.pop()?;
                    state.push(fun.push_insn(block, Insn::Send { self_val: left, call_info: CallInfo { name: "<<".into() }, args: vec![right] }));
                }
                YARVINSN_opt_aset => {
                    let set = state.pop()?;
                    let obj = state.pop()?;
                    let recv = state.pop()?;
                    fun.push_insn(block, Insn::Send { self_val: recv, call_info: CallInfo { name: "[]=".into() }, args: vec![obj, set] });
                    state.push(set);
                }

                YARVINSN_leave => {
                    fun.push_insn(block, Insn::Return { val: state.pop()? });
                    break;  // Don't enqueue the next block as a successor
                }

                YARVINSN_opt_send_without_block => {
                    let cd: *const rb_call_data = get_arg(pc, 0).as_ptr();
                    let call_info = unsafe { rb_get_call_data_ci(cd) };
                    let argc = unsafe { vm_ci_argc((*cd).ci) };


                    let method_name = unsafe {
                        let mid = rb_vm_ci_mid(call_info);
                        cstr_to_rust_string(rb_id2name(mid)).unwrap_or_else(|| "<unknown>".to_owned())
                    };
                    let mut args = vec![];
                    for _ in 0..argc {
                        args.push(state.pop()?);
                    }
                    args.reverse();

                    let recv = state.pop()?;
                    state.push(fun.push_insn(block, Insn::Send { self_val: recv, call_info: CallInfo { name: method_name }, args }));
                }
                _ => return Err(ParseError::UnknownOpcode(insn_name(opcode as usize))),
            }

            if insn_idx_to_block.contains_key(&insn_idx) {
                let target = insn_idx_to_block[&insn_idx];
                fun.push_insn(block, Insn::Jump(BranchEdge { target, args: state.as_args() }));
                queue.push_back((state, target, insn_idx));
                break;  // End the block
            }
        }
    }

    match get_option!(dump_hir) {
        Some(DumpHIR::WithoutSnapshot) => print!("HIR:\n{}", FunctionPrinter::without_snapshot(&fun)),
        Some(DumpHIR::All) => print!("HIR:\n{}", FunctionPrinter::with_snapshot(&fun)),
        Some(DumpHIR::Raw) => print!("HIR:\n{:#?}", &fun),
        None => {},
    }

    Ok(fun)
}

#[cfg(test)]
mod union_find_tests {
    use super::UnionFind;

    #[test]
    fn test_find_returns_self() {
        let mut uf = UnionFind::new();
        assert_eq!(uf.find(3usize), 3);
    }

    #[test]
    fn test_find_const_returns_target() {
        let mut uf = UnionFind::new();
        uf.make_equal_to(3, 4);
        assert_eq!(uf.find_const(3usize), 4);
    }

    #[test]
    fn test_find_const_returns_transitive_target() {
        let mut uf = UnionFind::new();
        uf.make_equal_to(3, 4);
        uf.make_equal_to(4, 5);
        assert_eq!(uf.find_const(3usize), 5);
        assert_eq!(uf.find_const(4usize), 5);
    }

    #[test]
    fn test_find_compresses_path() {
        let mut uf = UnionFind::new();
        uf.make_equal_to(3, 4);
        uf.make_equal_to(4, 5);
        assert_eq!(uf.at(3usize), Some(4));
        assert_eq!(uf.find(3usize), 5);
        assert_eq!(uf.at(3usize), Some(5));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[macro_export]
    macro_rules! assert_matches {
        ( $x:expr, $pat:pat ) => {
            {
                let val = $x;
                if (!matches!(val, $pat)) {
                    eprintln!("{} ({:?}) does not match pattern {}", stringify!($x), val, stringify!($pat));
                    assert!(false);
                }
            }
        };
    }

    #[test]
    fn boot_vm() {
        crate::cruby::with_rubyvm(|| {
            let program = "nil.itself";
            let iseq = compile_to_iseq(program);
            let function = iseq_to_hir(iseq).unwrap();
            assert!(matches!(function.insns.get(0), Some(Insn::Snapshot { .. })));
        });
    }

    #[test]
    fn test_putobject() {
        crate::cruby::with_rubyvm(|| {
            let program = "123";
            let iseq = compile_to_iseq(program);
            let function = iseq_to_hir(iseq).unwrap();
            assert_matches!(function.insns.get(1), Some(Insn::Const { val: VALUE(247) }));
            assert_matches!(function.insns.get(3), Some(Insn::Return { val: InsnId(1) }));
        });
    }

    #[test]
    fn test_opt_plus() {
        crate::cruby::with_rubyvm(|| {
            let program = "1+2";
            let iseq = compile_to_iseq(program);
            let function = iseq_to_hir(iseq).unwrap();
            // TODO(max): Figure out a clean way to match against String
            // TODO(max): Figure out a clean way to match against args vec
            assert_matches!(function.insns.get(1), Some(Insn::Const { val: VALUE(3) }));
            assert_matches!(function.insns.get(3), Some(Insn::Const { val: VALUE(5) }));
            assert_matches!(function.insns.get(5), Some(Insn::Send { self_val: InsnId(1), .. }));
        });
    }

    #[test]
    fn test_setlocal_getlocal() {
        crate::cruby::with_rubyvm(|| {
            let program = "a = 1; a";
            let iseq = compile_to_iseq(program);
            let function = iseq_to_hir(iseq).unwrap();
            assert_matches!(function.insns.get(2), Some(Insn::Const { val: VALUE(3) }));
            assert_matches!(function.insns.get(6), Some(Insn::Return { val: InsnId(2) }));
        });
    }

    #[test]
    fn test_merge_const() {
        crate::cruby::with_rubyvm(|| {
            let program = "cond = true; if cond; 3; else; 4; end";
            let iseq = compile_to_iseq(program);
            let function = iseq_to_hir(iseq).unwrap();
            assert_matches!(function.insns.get(2), Some(Insn::Const { val: Qtrue }));
            assert_matches!(function.insns.get(6), Some(Insn::Test { val: InsnId(2) }));
            assert_matches!(function.insns.get(7), Some(Insn::IfFalse { val: InsnId(6), target: BranchEdge { target: BlockId(1), .. } }));
            assert_matches!(function.insns.get(9), Some(Insn::Const { val: VALUE(7) }));
            assert_matches!(function.insns.get(11), Some(Insn::Return { val: InsnId(9) }));
            assert_matches!(function.insns.get(14), Some(Insn::Const { val: VALUE(9) }));
            assert_matches!(function.insns.get(16), Some(Insn::Return { val: InsnId(14) }));
        });
    }
}
