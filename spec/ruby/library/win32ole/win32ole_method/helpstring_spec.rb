require_relative "../../../spec_helper"
platform_is :windows do
  require 'win32ole'

  describe "WIN32OLE::Method#helpstring" do
    before :each do
      ole_type = WIN32OLE::Type.new("Microsoft Scripting Runtime", "File")
      @m_file_name = WIN32OLE::Method.new(ole_type, "name")
    end

    it "raises ArgumentError if argument is given" do
      -> { @m_file_name.helpstring(1) }.should raise_error ArgumentError
    end

    it "returns expected value for Scripting Runtime's 'File' method" do
      @m_file_name.helpstring.should == "Get name of file" # value indicated in MRI's test
    end

  end

end
