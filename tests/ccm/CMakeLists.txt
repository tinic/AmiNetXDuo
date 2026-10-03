# N-147/N-148: the vendored AES-CCM against independent known answers.
#
# HOST ONLY, as tests/x509.  Links tests/x509's nx_crypto archive, which is
# the whole of crypto_libraries/src under the shipping definitions, so this
# directory has to be added after x509.  The method objects in
# nx_crypto_methods.c are what TLS calls, and they reach every cipher, which
# is why the archive and not a source list.
#
#   python3 tests/ccm/gen_ccm_vectors.py > tests/ccm/ccm_vectors.h
#
# SPDX-License-Identifier: MIT

if(CMAKE_CROSSCOMPILING)
    return()
endif()

add_executable(test_ccm_contract test_ccm_contract.c)
target_include_directories(test_ccm_contract PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test_ccm_contract PRIVATE test_x509_nx_crypto)
target_compile_options(test_ccm_contract PRIVATE -Wall -Wextra -UAMINETXDUO_IPV6)

add_test(NAME ccm_contract COMMAND test_ccm_contract)
