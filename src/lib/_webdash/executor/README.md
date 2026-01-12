# WebDash Executor
Library used by binaries that wish to make use of the WebDash directory infrastructure, which is a hierarchical composition of directories with the ability to make use of WebDash configuration files (e.g., `webdash.config.json`). These configuration files enable various unique concepts, such as
- Directory-local environment variables.
- Use of "WebDash commands".
    - Introduced to remove the need of memorizing `bash` commands.
    - These wrap the execution of `bash`-native commands alongside their arguments. In a sense, introducing powerful command aliases.
        - Dependencies to other commands can be specified. E.g., build a library before building a binary that depends on it.
   - Given the name of command, WebDash can smartly identify a "best match" configuration file, with the ability to traverse through the ancestry of a directory.
   - With ease, allows the user to specify the `CWD` in which to run the command, including relative to where the `webdash.config.json` file is located.

## Note
- Style of CPP comments: https://developer.lsst.io/cpp/api-docs.html#cpp-doxygen-short-summary