# frozen_string_literal: true

require "bundler"
require "tmpdir"

RSpec.describe Bundler do
  describe "#load_marshal" do
    it "is a private method and raises an error" do
      data = Marshal.dump(Bundler)
      expect { Bundler.load_marshal(data) }.to raise_error(NoMethodError, /private method [`']load_marshal' called/)
    end

    it "loads any data" do
      data = Marshal.dump(Bundler)
      expect(Bundler.send(:load_marshal, data)).to eq(Bundler)
    end
  end

  describe "#safe_load_marshal" do
    it "fails on unexpected class" do
      data = Marshal.dump(Bundler)
      expect { Bundler.safe_load_marshal(data) }.to raise_error(Bundler::MarshalError)
    end

    it "loads simple structure" do
      simple_structure = { "name" => [:development] }
      data = Marshal.dump(simple_structure)
      expect(Bundler.safe_load_marshal(data)).to eq(simple_structure)
    end

    it "loads Gem::Specification" do
      gem_spec = Gem::Specification.new do |s|
        s.name = "bundler"
        s.version = Gem::Version.new("2.4.7")
        s.installed_by_version = Gem::Version.new("0")
        s.authors = ["André Arko",
                     "Samuel Giddins",
                     "Colby Swandale",
                     "Hiroshi Shibata",
                     "David Rodríguez",
                     "Grey Baker",
                     "Stephanie Morillo",
                     "Chris Morris",
                     "James Wen",
                     "Tim Moore",
                     "André Medeiros",
                     "Jessica Lynn Suttles",
                     "Terence Lee",
                     "Carl Lerche",
                     "Yehuda Katz"]
        s.date = Time.utc(2023, 2, 15)
        s.description = "Bundler manages an application's dependencies through its entire life, across many machines, systematically and repeatably"
        s.email = ["team@bundler.io"]
        s.homepage = "https://bundler.io"
        s.metadata = { "bug_tracker_uri" => "https://github.com/rubygems/rubygems/issues?q=is%3Aopen+is%3Aissue+label%3ABundler",
                       "changelog_uri" => "https://github.com/rubygems/rubygems/blob/master/bundler/CHANGELOG.md",
                       "homepage_uri" => "https://bundler.io/",
                       "source_code_uri" => "https://github.com/rubygems/rubygems/tree/master/bundler" }
        s.require_paths = ["lib"]
        s.required_ruby_version = Gem::Requirement.new([">= 2.6.0"])
        s.required_rubygems_version = Gem::Requirement.new([">= 3.0.1"])
        s.rubygems_version = "3.4.7"
        s.specification_version = 4
        s.summary = "The best way to manage your application's dependencies"
        s.license = false
      end
      data = Marshal.dump(gem_spec)
      expect(Bundler.safe_load_marshal(data)).to eq(gem_spec)
    end
  end

  describe "#load_gemspec_uncached" do
    let(:app_gemspec_path) { tmp("test.gemspec") }
    subject { Bundler.load_gemspec_uncached(app_gemspec_path) }

    context "with incorrect YAML file" do
      before do
        File.open(app_gemspec_path, "wb") do |f|
          f.write <<~GEMSPEC
            ---
              {:!00 ao=gu\g1= 7~f
          GEMSPEC
        end
      end

      it "catches YAML syntax errors" do
        expect { subject }.to raise_error(Bundler::GemspecError, /error while loading `test.gemspec`/)
      end
    end

    context "with correct YAML file", if: defined?(Encoding) do
      it "can load a gemspec with unicode characters with default ruby encoding" do
        # spec_helper forces the external encoding to UTF-8 but that's not the
        # default until Ruby 2.0
        verbose = $VERBOSE
        $VERBOSE = false
        encoding = Encoding.default_external
        Encoding.default_external = "ASCII"
        $VERBOSE = verbose

        File.open(app_gemspec_path, "wb") do |file|
          file.puts <<~GEMSPEC
            # -*- encoding: utf-8 -*-
            Gem::Specification.new do |gem|
              gem.author = "André the Giant"
            end
          GEMSPEC
        end

        expect(subject.author).to eq("André the Giant")

        verbose = $VERBOSE
        $VERBOSE = false
        Encoding.default_external = encoding
        $VERBOSE = verbose
      end
    end

    it "sets loaded_from" do
      app_gemspec_path.open("w") do |f|
        f.puts <<-GEMSPEC
          Gem::Specification.new do |gem|
            gem.name = "validated"
          end
        GEMSPEC
      end

      expect(subject.loaded_from).to eq(app_gemspec_path.expand_path.to_s)
    end

    context "validate is true" do
      subject { Bundler.load_gemspec_uncached(app_gemspec_path, true) }

      it "validates the specification" do
        app_gemspec_path.open("w") do |f|
          f.puts <<-GEMSPEC
            Gem::Specification.new do |gem|
              gem.name = "validated"
            end
          GEMSPEC
        end
        expect(Bundler.rubygems).to receive(:validate).with have_attributes(name: "validated")
        subject
      end
    end

    context "with gemspec containing local variables" do
      before do
        File.open(app_gemspec_path, "wb") do |f|
          f.write <<~GEMSPEC
            must_not_leak = true
            Gem::Specification.new do |gem|
              gem.name = "leak check"
            end
          GEMSPEC
        end
      end

      it "should not pollute the TOPLEVEL_BINDING" do
        subject
        expect(TOPLEVEL_BINDING.eval("local_variables")).to_not include(:must_not_leak)
      end
    end
  end

  describe "#which" do
    it "can detect relative path" do
      script_path = bundled_app("tmp/test_command")
      create_file(script_path, "#!/usr/bin/env ruby\n")

      result = Dir.chdir script_path.dirname.dirname do
        Bundler.which("test_command")
      end
      expect(result).to eq(nil)

      result = Dir.chdir script_path.dirname do
        Bundler.which("test_command")
      end

      expect(result).to eq("test_command") unless Gem.win_platform?
      expect(result).to eq("test_command.bat") if Gem.win_platform?
    end

    it "can detect absolute path" do
      create_file("test_command", "#!/usr/bin/env ruby\n")

      ENV["PATH"] = bundled_app("test_command").parent.to_s

      result = Bundler.which("test_command")
      expect(result).to eq(bundled_app("test_command").to_s) unless Gem.win_platform?
      expect(result).to eq(bundled_app("test_command.bat").to_s) if Gem.win_platform?
    end

    it "returns nil when not found" do
      result = Bundler.which("test_command")
      expect(result).to eq(nil)
    end
  end

  describe "configuration" do
    context "disable_shared_gems" do
      it "should unset GEM_PATH with empty string" do
        expect(Bundler).to receive(:use_system_gems?).and_return(false)
        Bundler.send(:configure_gem_path)
        expect(ENV["GEM_PATH"]).to eq ""
      end
    end
  end

  describe "#mkdir_p" do
    it "creates a folder at the given path" do
      install_gemfile <<-G
        source "https://gem.repo1"
        gem "myrack"
      G

      allow(Bundler).to receive(:root).and_return(bundled_app)

      Bundler.mkdir_p(bundled_app("foo", "bar"))
      expect(bundled_app("foo", "bar")).to exist
    end
  end

  describe "#user_home" do
    context "home directory is set" do
      it "should return the user home" do
        path = "/home/oggy"
        allow(Bundler.rubygems).to receive(:user_home).and_return(path)
        allow(File).to receive(:directory?).with(path).and_return true
        allow(File).to receive(:writable?).with(path).and_return true
        expect(Bundler.user_home).to eq(Pathname(path))
      end

      context "is not a directory" do
        it "should issue a warning and return a temporary user home" do
          path = "/home/oggy"
          allow(Bundler.rubygems).to receive(:user_home).and_return(path)
          allow(File).to receive(:directory?).with(path).and_return false
          allow(Bundler).to receive(:tmp).and_return(Pathname.new("/tmp/trulyrandom"))
          expect(Bundler.ui).to receive(:warn).with("`/home/oggy` is not a directory.\n")
          expect(Bundler.ui).to receive(:warn).with("Bundler will use `/tmp/trulyrandom' as your home directory temporarily.\n")
          expect(Bundler.user_home).to eq(Pathname("/tmp/trulyrandom"))
        end
      end

      context "is not writable" do
        let(:path) { "/home/oggy" }
        let(:dotbundle) { "/home/oggy/.bundle" }

        it "should issue a warning and return a temporary user home" do
          allow(Bundler.rubygems).to receive(:user_home).and_return(path)
          allow(File).to receive(:directory?).with(path).and_return true
          allow(File).to receive(:writable?).and_call_original
          allow(File).to receive(:writable?).with(path).and_return false
          allow(File).to receive(:directory?).with(dotbundle).and_return false
          allow(Bundler).to receive(:tmp).and_return(Pathname.new("/tmp/trulyrandom"))
          expect(Bundler.ui).to receive(:warn).with("`/home/oggy` is not writable.\n")
          expect(Bundler.ui).to receive(:warn).with("Bundler will use `/tmp/trulyrandom' as your home directory temporarily.\n")
          expect(Bundler.user_home).to eq(Pathname("/tmp/trulyrandom"))
        end

        context ".bundle exists and have correct permissions" do
          it "should return the user home" do
            allow(Bundler.rubygems).to receive(:user_home).and_return(path)
            allow(File).to receive(:directory?).with(path).and_return true
            allow(File).to receive(:writable?).with(path).and_return false
            allow(File).to receive(:directory?).with(dotbundle).and_return true
            allow(File).to receive(:writable?).with(dotbundle).and_return true
            expect(Bundler.user_home).to eq(Pathname(path))
          end
        end
      end
    end

    context "home directory is not set" do
      it "should issue warning and return a temporary user home" do
        allow(Bundler.rubygems).to receive(:user_home).and_return(nil)
        allow(Bundler).to receive(:tmp).and_return(Pathname.new("/tmp/trulyrandom"))
        expect(Bundler.ui).to receive(:warn).with("Your home directory is not set.\n")
        expect(Bundler.ui).to receive(:warn).with("Bundler will use `/tmp/trulyrandom' as your home directory temporarily.\n")
        expect(Bundler.user_home).to eq(Pathname("/tmp/trulyrandom"))
      end
    end
  end

  context "user cache dir" do
    let(:home_path)                  { Pathname.new(ENV["HOME"]) }

    let(:xdg_data_home)              { home_path.join(".local") }
    let(:xdg_cache_home)             { home_path.join(".cache") }
    let(:xdg_config_home)            { home_path.join(".config") }

    let(:bundle_user_home_default)   { home_path.join(".bundle") }
    let(:bundle_user_home_custom)    { xdg_data_home.join("bundle") }

    let(:bundle_user_cache_default)  { bundle_user_home_default.join("cache") }
    let(:bundle_user_cache_custom)   { xdg_cache_home.join("bundle") }

    let(:bundle_user_config_default) { bundle_user_home_default.join("config") }
    let(:bundle_user_config_custom)  { xdg_config_home.join("bundle") }

    let(:bundle_user_plugin_default) { bundle_user_home_default.join("plugin") }
    let(:bundle_user_plugin_custom)  { xdg_data_home.join("bundle").join("plugin") }

    describe "#user_bundle_path" do
      before do
        allow(Bundler.rubygems).to receive(:user_home).and_return(home_path)
      end

      it "should use the default home path" do
        expect(Bundler.user_bundle_path).to           eq(bundle_user_home_default)
        expect(Bundler.user_bundle_path("home")).to   eq(bundle_user_home_default)
        expect(Bundler.user_bundle_path("cache")).to  eq(bundle_user_cache_default)
        expect(Bundler.user_cache).to                 eq(bundle_user_cache_default)
        expect(Bundler.user_bundle_path("config")).to eq(bundle_user_config_default)
        expect(Bundler.user_bundle_path("plugin")).to eq(bundle_user_plugin_default)
      end

      it "should use custom home path as root for other paths" do
        ENV["BUNDLE_USER_HOME"] = bundle_user_home_custom.to_s
        allow(Bundler.rubygems).to receive(:user_home).and_raise
        expect(Bundler.user_bundle_path).to           eq(bundle_user_home_custom)
        expect(Bundler.user_bundle_path("home")).to   eq(bundle_user_home_custom)
        expect(Bundler.user_bundle_path("cache")).to  eq(bundle_user_home_custom.join("cache"))
        expect(Bundler.user_cache).to                 eq(bundle_user_home_custom.join("cache"))
        expect(Bundler.user_bundle_path("config")).to eq(bundle_user_home_custom.join("config"))
        expect(Bundler.user_bundle_path("plugin")).to eq(bundle_user_home_custom.join("plugin"))
      end

      it "should use all custom paths, except home" do
        ENV.delete("BUNDLE_USER_HOME")
        ENV["BUNDLE_USER_CACHE"]  = bundle_user_cache_custom.to_s
        ENV["BUNDLE_USER_CONFIG"] = bundle_user_config_custom.to_s
        ENV["BUNDLE_USER_PLUGIN"] = bundle_user_plugin_custom.to_s
        expect(Bundler.user_bundle_path).to           eq(bundle_user_home_default)
        expect(Bundler.user_bundle_path("home")).to   eq(bundle_user_home_default)
        expect(Bundler.user_bundle_path("cache")).to  eq(bundle_user_cache_custom)
        expect(Bundler.user_cache).to                 eq(bundle_user_cache_custom)
        expect(Bundler.user_bundle_path("config")).to eq(bundle_user_config_custom)
        expect(Bundler.user_bundle_path("plugin")).to eq(bundle_user_plugin_custom)
      end
    end
  end
end
