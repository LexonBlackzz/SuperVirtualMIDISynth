# Writes a CUDA fat binary as a C array header: cmake -DBIN2C=<bin2c> -DIN=<file>
# -DOUT=<header> -DNAME=<array> -P EmbedFatbin.cmake  (bin2c prints to stdout).
execute_process(
    COMMAND "${BIN2C}" --const --name "${NAME}" "${IN}"
    OUTPUT_FILE "${OUT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    file(REMOVE "${OUT}")
    message(FATAL_ERROR "bin2c failed (${result})")
endif()
