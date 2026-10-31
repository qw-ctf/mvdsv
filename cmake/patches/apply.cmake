# Applies PATCH to the git checkout in the current directory, once: a patch
# already in is left alone, so that the step can run again.
#
#   cmake -DGIT=<git> -DPATCH=<file> -P apply.cmake

execute_process(COMMAND "${GIT}" apply --reverse --check "${PATCH}"
	RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(applied EQUAL 0)
	message(STATUS "Already applied: ${PATCH}")
	return()
endif()

execute_process(COMMAND "${GIT}" apply "${PATCH}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "${PATCH} does not apply")
endif()
message(STATUS "Applied: ${PATCH}")
