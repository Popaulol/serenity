serenity_component(
    Shell
    REQUIRED
    TARGETS Shell
)

set(SOURCES
    AST.cpp
    Builtin.cpp
    Formatter.cpp
    ImmediateFunctions.cpp
    Job.cpp
    NodeVisitor.cpp
    Parser.cpp
    PosixLexer.cpp
    PosixParser.cpp
    Shell.cpp
)

serenity_lib(LibShell shell)
target_link_libraries(LibShell PRIVATE LibCore LibFileSystem LibLine LibSyntax LibRegex)

if (SERENITYOS)
    target_sources(LibShell PRIVATE SyntaxHighlighter.cpp)
    target_link_libraries(LibShell PRIVATE LibCodeComprehension)
endif()

if (ENABLE_FUZZERS)
    add_custom_target(Shell) # Stub target to placate serenity_component
else()
    set(SOURCES main.cpp)

    serenity_bin(Shell)
    target_link_libraries(Shell PRIVATE LibCore LibFileSystem LibLine LibShell LibMain)

    install(DIRECTORY Tests/ DESTINATION usr/Tests/Shell
            PATTERN "Tests/*"
            PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
            GROUP_EXECUTE GROUP_READ
            WORLD_EXECUTE WORLD_READ)
endif()
