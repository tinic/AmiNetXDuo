# What the host CI stage has to build, derived from what is registered.
#
# THE DEFECT THIS EXISTS TO END.  tools/ci.sh used to carry a hand-written
# list of targets to build before running ctest.  A test whose binary was not
# named in that list passed on every developer's machine -- where the binary
# was already lying in the build tree from an earlier `--target all` or an
# earlier branch -- and failed on every clean configure in CI, as "Not Run".
# Three times:
#
#   the netdev host tests    missing ${CMAKE_SOURCE_DIR}/include, so they only
#                            failed on a fresh configure
#   rfbgen, rfbbench,        never in the list at all; rfb_roundtrip ran
#   rfbwords                 binaries nothing had built
#   test_netdev_el3          same again, 2026-08
#
# Three occurrences of one shape is not bad luck, it is a list that has no
# mechanical relationship to the thing it is supposed to mirror.  So there is
# no list any more.  add_test() is wrapped, every registration is recorded,
# and the build targets are computed from the registrations at the end of
# configure and written out for tools/ci.sh to read.  The two cannot disagree
# because there is only one of them.
#
# WHAT IS DERIVED, AND HOW
#
#   the COMMAND word            add_test(NAME x COMMAND test_x) -- if the word
#                               names a target, that target is what x needs
#   $<TARGET_FILE:y> anywhere   in the COMMAND's arguments.  This is what
#                               catches an INGREDIENT: rfb_roundtrip runs
#                               `cmake -P roundtrip.cmake` and passes it
#                               -DGEN=$<TARGET_FILE:rfbgen>, so rfbgen is
#                               named in the registration even though it is
#                               not the command.  Nothing had to be declared
#                               twice for that to work.
#
# A test whose command is a script that finds its helpers some other way than
# $<TARGET_FILE:> is not derivable and would be missed.  Nothing in this tree
# does that, and the fix if something ever does is to pass the path in, which
# is what a test should do anyway rather than guessing at a build layout.
#
# Include this BEFORE the first add_subdirectory(), or the registrations in
# whatever came first are not seen.
#
# SPDX-License-Identifier: MIT

# CMake makes the original available as _add_test when a function of the same
# name is defined.  Every add_test() in the tree goes through here with no
# call site changed, which is the point: a rule that has to be remembered at
# the call site is the rule that was already being forgotten.
#
# Each registration is stored as ONE list item, semicolons escaped, so the
# writer can walk them one test at a time.  Flattening them all into a single
# bag of words would work today and would mis-read the first test that passed
# the literal word COMMAND as an argument, running the scan on into the next
# registration.
#
function(add_test)
    _add_test(${ARGV})

    string(REPLACE ";" "\\;" _one "${ARGV}")
    set_property(GLOBAL APPEND PROPERTY ANX_TEST_REGISTRATIONS "${_one}")
endfunction()

#
# Resolve the recorded registrations into a target list, and fail the
# configure on a registration that cannot name a runnable command.
#
# THE COMMAND RULE, WHICH NEEDS NO LIST OF INTERPRETERS.  A command is either
# a target, or something with a "/" or a "$" in it -- an absolute path, a
# generated path, a generator expression.  A bare word that is not a target is
# a typo or a target that was never defined, and it is a FATAL_ERROR here
# rather than a "Could not find executable" from ctest an hour later.
#
function(anx_write_host_test_targets out_file)
    get_property(_regs GLOBAL PROPERTY ANX_TEST_REGISTRATIONS)

    set(_targets "")
    set(_tests 0)

    foreach(_reg IN LISTS _regs)
        math(EXPR _tests "${_tests} + 1")

        # add_test(NAME <n> COMMAND <cmd> ...), which is the only form this
        # tree uses.  The old add_test(<n> <cmd> ...) form has no COMMAND
        # keyword; it is rejected rather than half-understood.
        list(GET _reg 0 _kw)
        if(NOT _kw STREQUAL "NAME")
            message(FATAL_ERROR
                "add_test(${_reg}) uses the old positional form.  Use "
                "add_test(NAME <name> COMMAND <command> ...) so that what CI "
                "has to build can be derived from it.")
        endif()

        list(GET _reg 1 _name)

        list(FIND _reg "COMMAND" _ci)
        if(_ci LESS 0)
            message(FATAL_ERROR "add_test(${_name}) has no COMMAND.")
        endif()

        # The command itself.
        math(EXPR _ci "${_ci} + 1")
        list(GET _reg ${_ci} _cmd)
        if(TARGET "${_cmd}")
            list(APPEND _targets "${_cmd}")
        elseif(_cmd MATCHES "[/$]")
            # A path or a generator expression: not a target, and not
            # something this can or should check the existence of.
        else()
            message(FATAL_ERROR
                "add_test(${_name}) runs \"${_cmd}\", which is neither a "
                "target nor a path.  If it is a program, give its full path; "
                "if it is a target, it was never defined.")
        endif()

        # Ingredients, anywhere in the arguments.
        foreach(_w IN LISTS _reg)
            string(REGEX MATCHALL "\\$<TARGET_FILE:[A-Za-z0-9_.+-]+>"
                   _refs "${_w}")
            foreach(_r IN LISTS _refs)
                string(REGEX REPLACE "^\\$<TARGET_FILE:(.*)>$" "\\1" _t "${_r}")
                if(NOT TARGET "${_t}")
                    message(FATAL_ERROR
                        "add_test(${_name}) refers to $<TARGET_FILE:${_t}> "
                        "but no target ${_t} exists.")
                endif()
                list(APPEND _targets "${_t}")
            endforeach()
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES _targets)
    list(SORT _targets)

    list(LENGTH _targets _n)
    message(STATUS
        "  host tests .... ${_tests} registered, ${_n} targets to build")

    string(REPLACE ";" "\n" _text "${_targets}")
    file(WRITE "${out_file}" "${_text}\n")
endfunction()
