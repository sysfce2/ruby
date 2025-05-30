# encoding: binary
require_relative '../../spec_helper'

describe "Encoding#replicate" do
  ruby_version_is ""..."3.3" do
    before :all do
      @i = 0
    end

    before :each do
      @i += 1
      @prefix = "RS#{@i}"
    end

    it "returns a replica of ASCII" do
      name = @prefix + '-ASCII'
      e = suppress_warning { Encoding::ASCII.replicate(name) }
      e.name.should == name
      Encoding.find(name).should == e

      "a".dup.force_encoding(e).valid_encoding?.should be_true
      "\x80".dup.force_encoding(e).valid_encoding?.should be_false
    end

    it "returns a replica of UTF-8" do
      name = @prefix + 'UTF-8'
      e = suppress_warning { Encoding::UTF_8.replicate(name) }
      e.name.should == name
      Encoding.find(name).should == e

      "a".dup.force_encoding(e).valid_encoding?.should be_true
      "\u3042".dup.force_encoding(e).valid_encoding?.should be_true
      "\x80".dup.force_encoding(e).valid_encoding?.should be_false
    end

    it "returns a replica of UTF-16BE" do
      name = @prefix + 'UTF-16-BE'
      e = suppress_warning { Encoding::UTF_16BE.replicate(name) }
      e.name.should == name
      Encoding.find(name).should == e

      "a".dup.force_encoding(e).valid_encoding?.should be_false
      "\x30\x42".dup.force_encoding(e).valid_encoding?.should be_true
      "\x80".dup.force_encoding(e).valid_encoding?.should be_false
    end

    it "returns a replica of ISO-2022-JP" do
      name = @prefix + 'ISO-2022-JP'
      e = suppress_warning { Encoding::ISO_2022_JP.replicate(name) }
      Encoding.find(name).should == e

      e.name.should == name
      e.dummy?.should be_true
    end

    # NOTE: it's unclear of the value of this (for the complexity cost of it),
    # but it is the current CRuby behavior.
    it "can be associated with a String" do
      name = @prefix + '-US-ASCII'
      e = suppress_warning { Encoding::US_ASCII.replicate(name) }
      e.name.should == name
      Encoding.find(name).should == e

      s = "abc".dup.force_encoding(e)
      s.encoding.should == e
      s.encoding.name.should == name
    end
  end

  ruby_version_is ""..."3.3" do
    it "warns about deprecation" do
      -> {
        Encoding::US_ASCII.replicate('MY-US-ASCII')
      }.should complain(/warning: Encoding#replicate is deprecated and will be removed in Ruby 3.3; use the original encoding instead/)
    end

    it "raises EncodingError if too many encodings" do
      code = '1_000.times {|i| Encoding::US_ASCII.replicate("R_#{i}") }'
      ruby_exe(code, args: "2>&1", exit_status: 1).should.include?('too many encoding (> 256) (EncodingError)')
    end
  end

  ruby_version_is "3.3" do
    it "has been removed" do
      Encoding::US_ASCII.should_not.respond_to?(:replicate, true)
    end
  end
end
