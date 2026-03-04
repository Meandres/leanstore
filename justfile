proot := source_dir()
build_dir := "build"
user := `whoami`

build type="Release":
    #!/usr/bin/env bash
    cmake -S {{proot}} -B {{build_dir}} \
        -DCMAKE_BUILD_TYPE={{type}} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build {{build_dir}} --parallel $(nproc)

run_frontend:
    #!/usr/bin/env bash
    {{proot}}/{{build_dir}}/frontend/frontend

run_ycsb:
    #!/usr/bin/env bash
    {{proot}}/{{build_dir}}/frontend/ycsb

run_tpcc:
    #!/usr/bin/env bash
    {{proot}}/{{build_dir}}/frontend/tpcc