require_relative "../../../spec_helper"
require_relative 'shared/name'

platform_is :windows do
  verbose, $VERBOSE = $VERBOSE, nil

  require 'win32ole'

  describe "WIN32OLE_PARAM#to_s" do
    it_behaves_like :win32ole_param_name, :to_s

  end

ensure
  $VERBOSE = verbose
end
