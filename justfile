proot := source_dir()
build_dir := "build"
user := `whoami`

build type="Release":
    #!/usr/bin/env bash
    cmake -S {{proot}} -B {{build_dir}} \
        -DCMAKE_BUILD_TYPE={{type}} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build {{build_dir}} --parallel $(nproc)

run_mvcc_overhead:
    #!/usr/bin/env bash
    {{proot}}/{{build_dir}}/benchmark/MicroMVCCReadOverhead --record_count=200000 --warmup_iters=2 --bench_iters=5