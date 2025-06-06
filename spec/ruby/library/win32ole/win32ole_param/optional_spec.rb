require_relative "../../../spec_helper"
platform_is :windows do
  require 'win32ole'

  describe "WIN32OLE::Param#optional?" do
    before :each do
      ole_type_detail = WIN32OLE::Type.new("Microsoft Scripting Runtime", "FileSystemObject")
      m_copyfile = WIN32OLE::Method.new(ole_type_detail, "CopyFile")
      @param_overwritefiles = m_copyfile.params[2]
    end

    it "raises ArgumentError if argument is given" do
      -> { @param_overwritefiles.optional?(1) }.should raise_error ArgumentError
    end

    it "returns true for 3rd parameter of FileSystemObject's 'CopyFile' method" do
      @param_overwritefiles.optional?.should be_true
    end

  end

end
