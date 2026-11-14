execute_process(COMMAND ${EXE} --ecp-describe OUTPUT_FILE ${OUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${EXE} --ecp-describe failed")
endif()
