require_relative "../../../spec_helper"
platform_is :windows do
  require 'win32ole'

  describe "WIN32OLE::Method#invoke_kind" do
    before :each do
      ole_type = WIN32OLE::Type.new("Microsoft Scripting Runtime", "File")
      @m_file_name = WIN32OLE::Method.new(ole_type, "name")
    end

    it "raises ArgumentError if argument is given" do
      -> { @m_file_name.invoke_kind(1) }.should raise_error ArgumentError
    end

    it "returns expected value for Scripting Runtime's 'name' method" do
      @m_file_name.invoke_kind.should =~ /^(UNKNOWN|PROPERTY|PROPERTYGET|PROPERTYPUT|PROPERTYPUTREF|FUNC)$/
    end

  end

end
