require_relative "../../../spec_helper"
platform_is :windows do
  verbose, $VERBOSE = $VERBOSE, nil

  require 'win32ole'

  describe "WIN32OLE_TYPE.typelibs for Shell Controls" do
    before :each do
      @ole_type = WIN32OLE_TYPE.new("Microsoft Shell Controls And Automation", "Shell")
    end

    after :each do
      @ole_type = nil
    end

    it "raises ArgumentError if any argument is give" do
      -> { WIN32OLE_TYPE.typelibs(1) }.should raise_error ArgumentError
    end

    it "returns array of type libraries" do
      WIN32OLE_TYPE.typelibs().include?("Microsoft Shell Controls And Automation").should be_true
    end

  end
ensure
  $VERBOSE = verbose
end
