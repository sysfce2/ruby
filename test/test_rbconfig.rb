# frozen_string_literal: false
require 'test/unit'
require 'rbconfig'
require 'shellwords'

class TestRbConfig < Test::Unit::TestCase
  @@with_config = {}

  Shellwords::shellwords(RbConfig::CONFIG["configure_args"]).grep(/\A--with-([^=]*)=(.*)/) do
    @@with_config[$1.tr('_', '-')] = $2
  end

  def test_sitedirs
    RbConfig::MAKEFILE_CONFIG.each do |key, val|
      next unless /\Asite(?!arch)/ =~ key
      next if @@with_config[key]
      assert_match(/(?:\$\(|\/)site/, val, key)
    end
  end

  def test_vendordirs
    RbConfig::MAKEFILE_CONFIG.each do |key, val|
      next unless /\Avendor(?!arch)/ =~ key
      next if @@with_config[key]
      assert_match(/(?:\$\(|\/)vendor/, val, key)
    end
  end

  def test_archdirs
    RbConfig::MAKEFILE_CONFIG.each do |key, val|
      next unless /\A(?!site|vendor|archdir\z).*arch.*dir\z/ =~ key
      next if @@with_config[key]
      assert_match(/\$\(arch|\$\(rubyarchprefix\)/, val, key)
    end
  end

  def test_sitearchdirs
    bug7823 = '[ruby-dev:46964] [Bug #7823]'
    RbConfig::MAKEFILE_CONFIG.each do |key, val|
      next unless /\Asite.*arch.*dir\z/ =~ key
      next if @@with_config[key]
      assert_match(/\$\(sitearch|\$\(rubysitearchprefix\)/, val, "#{key} #{bug7823}")
    end
  end

  def test_vendorarchdirs
    bug7823 = '[ruby-dev:46964] [Bug #7823]'
    RbConfig::MAKEFILE_CONFIG.each do |key, val|
      next unless /\Avendor.*arch.*dir\z/ =~ key
      next if @@with_config[key]
      assert_match(/\$\(sitearch|\$\(rubysitearchprefix\)/, val, "#{key} #{bug7823}")
    end
  end

  def test_limits_and_sizeof_access_in_ractor
    assert_separately(["-W0"], <<~'RUBY')
      r = Ractor.new do
        sizeof_int = RbConfig::SIZEOF["int"]
        fixnum_max = RbConfig::LIMITS["FIXNUM_MAX"]
        [sizeof_int, fixnum_max]
      end

      sizeof_int, fixnum_max = r.value

      assert_kind_of Integer, sizeof_int, "RbConfig::SIZEOF['int'] should be an Integer"
      assert_kind_of Integer, fixnum_max, "RbConfig::LIMITS['FIXNUM_MAX'] should be an Integer"
    RUBY
  end if defined?(Ractor)
end
