use std::env;
use std::path::{Path, PathBuf};

fn main() {
    // This crate lives at <repo>/rust/ac3forge-sys, so two levels up is the CMake source
    // directory that owns src/capi/ - see rust/README.md for why this crate builds the C
    // library itself (bindgen against a header that was NOT built into the library it links
    // is exactly the drift AP9 exists to catch, so there is no "assume it's preinstalled" path).
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let repo_root = manifest_dir
        .parent()
        .and_then(Path::parent)
        .expect("rust/ac3forge-sys must live two directories below the repo root")
        .to_path_buf();

    for rel in ["src/capi", "src/forge/include", "CMakeLists.txt"] {
        println!("cargo:rerun-if-changed={}", repo_root.join(rel).display());
    }

    // Same trimmed option set python/pyproject.toml already uses for its own extension-module
    // build: nothing outside ac3::forge_c and the codec core it embeds is relevant to this
    // binding, and turning the rest off keeps this build fast. {fmt} - the one dependency the
    // codec core itself has - resolves via find_package(CONFIG) with a FetchContent fallback
    // (cmake/Fmt.cmake), so no vcpkg toolchain file is needed here.
    let mut config = cmake::Config::new(&repo_root);
    if cfg!(target_env = "msvc") {
        // cmake-rs passes its own -DCMAKE_CXX_FLAGS=... on the configure command line, which
        // pre-empts CMake's usual CMAKE_CXX_FLAGS_INIT seeding (Modules/Platform/Windows-MSVC.cmake)
        // - the only place /EHsc would otherwise come from. Nothing in this project's own
        // CMakeLists.txt sets it explicitly (every desktop preset instead lets that CMake default
        // through), so without this the codec core fails to compile under MSVC with C4530
        // ("C++ exception handler used, but unwind semantics are not enabled") escalated to a
        // hard error by ac3::warnings' /WX.
        config.cxxflag("/EHsc");
    }
    config
        .define("AC3FORGE_BUILD_CAPI", "ON")
        .define("AC3FORGE_BUILD_CLI", "OFF")
        .define("AC3FORGE_BUILD_GUI", "OFF")
        .define("AC3FORGE_BUILD_TESTS", "OFF")
        .define("AC3FORGE_BUILD_EXAMPLES", "OFF")
        .define("AC3FORGE_BUILD_FUZZERS", "OFF")
        .define("AC3FORGE_BUILD_ADM", "OFF")
        .define("AC3FORGE_BUILD_MATROSKA", "OFF")
        .define("AC3FORGE_BUILD_MP4", "OFF")
        .define("AC3FORGE_BUILD_MPEGTS", "OFF")
        .build_target("forge_c_shared");
    // `dst` is cmake-rs's own OUT_DIR-rooted prefix; the actual CMake build tree (what a plain
    // `cmake -B <dir>` would call the binary dir) lives at `<dst>/build` by cmake-rs convention.
    // Deliberately NOT calling `cmake --install`: the project's install() rules cover the whole
    // configured project (including ac3::forge_shared's own .dll), not just the one target this
    // build actually built, so a full install fails trying to copy artifacts that were never
    // compiled here. The header is plain source (no generation needed) and the compiled
    // artifacts are found directly in the build tree below instead.
    let dst = config.build();
    let build_dir = dst.join("build");

    let source_include_dir = repo_root.join("src").join("capi").join("include");
    let header = source_include_dir.join("ac3forge_c").join("ac3forge.h");
    assert!(
        header.is_file(),
        "expected the C API header at {}",
        header.display()
    );

    // generate_export_header() (src/capi/CMakeLists.txt) writes export.h under this target's own
    // binary dir at configure/build time - a real generated file, not something `cmake --install`
    // is needed for either.
    let generated_include_dir = find_dir_containing(&build_dir, Path::new("ac3forge_c/export.h"))
        .expect("could not find generated ac3forge_c/export.h under the CMake build tree");

    let (runtime_name, import_lib_name): (&str, Option<&str>) = if cfg!(target_os = "windows") {
        ("ac3forge_c.dll", Some("ac3forge_c.lib"))
    } else if cfg!(target_os = "macos") {
        ("libac3forge_c.dylib", None)
    } else {
        ("libac3forge_c.so", None)
    };
    let runtime_lib = find_file(&build_dir, runtime_name).unwrap_or_else(|| {
        panic!(
            "could not find {runtime_name} under {}",
            build_dir.display()
        )
    });
    let link_search_dir = match import_lib_name {
        Some(name) => find_file(&build_dir, name)
            .unwrap_or_else(|| panic!("could not find {name} under {}", build_dir.display()))
            .parent()
            .unwrap()
            .to_path_buf(),
        None => runtime_lib.parent().unwrap().to_path_buf(),
    };

    println!(
        "cargo:rustc-link-search=native={}",
        link_search_dir.display()
    );
    println!("cargo:rustc-link-lib=dylib=ac3forge_c");

    copy_runtime_library(&runtime_lib, runtime_name);

    let bindings = bindgen::Builder::default()
        .header(header.to_str().unwrap())
        // AP4 proved this exact header compiles clean as strict C11
        // (C_STANDARD 11, C_EXTENSIONS OFF, -Wpedantic) on every desktop leg - bindgen parses
        // the same dialect the project itself guarantees, not an unpinned default.
        .clang_arg("-std=c11")
        .clang_arg(format!("-I{}", source_include_dir.display()))
        .clang_arg(format!("-I{}", generated_include_dir.display()))
        .allowlist_function("ac3forge_.*")
        .allowlist_type("ac3forge_.*")
        .allowlist_var("AC3FORGE_.*")
        .derive_default(true)
        .derive_debug(true)
        .generate()
        .expect("bindgen failed to generate ac3forge_c bindings");

    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("failed to write bindgen output");
}

/// Copies the built shared library next to this build's Cargo output so `cargo test`/`cargo run
/// --example` find it without the caller hand-managing LD_LIBRARY_PATH/PATH. This is a
/// local-dev/CI convenience, not a deployment story - see rust/README.md.
fn copy_runtime_library(src_lib: &Path, runtime_name: &str) {
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    // OUT_DIR is target/<profile>/build/<pkg>-<hash>/out; three levels up is target/<profile>/,
    // where `cargo test`'s and `cargo run --example`'s own executables land (in deps/ and
    // examples/ respectively - both copied into below on Windows, since DLL search only checks
    // the loading exe's own directory and PATH, not an arbitrary ancestor).
    let profile_dir = out_dir
        .ancestors()
        .nth(3)
        .expect("OUT_DIR should be target/<profile>/build/<pkg>/out")
        .to_path_buf();

    if cfg!(target_os = "windows") {
        for sub in ["", "deps", "examples"] {
            let target_dir = if sub.is_empty() {
                profile_dir.clone()
            } else {
                profile_dir.join(sub)
            };
            std::fs::create_dir_all(&target_dir).ok();
            let _ = std::fs::copy(src_lib, target_dir.join(runtime_name));
        }
    } else {
        std::fs::create_dir_all(&profile_dir).ok();
        let _ = std::fs::copy(src_lib, profile_dir.join(runtime_name));
        // No Windows equivalent needed: the copy loop above already puts the DLL in the
        // directory Windows actually searches (the loading exe's own directory).
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", profile_dir.display());
    }
}

/// Recursively finds a file named exactly `name` under `root`. The CMake build tree's exact
/// layout (a flat `bin/`, a per-target subdirectory, `Debug`/`Release` nesting on a multi-config
/// generator) varies by generator and platform, so this searches rather than assumes one shape -
/// robust to Ninja (single-config) and Visual Studio (multi-config) alike.
fn find_file(root: &Path, name: &str) -> Option<PathBuf> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let entries = match std::fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.file_name().and_then(|n| n.to_str()) == Some(name) {
                return Some(path);
            }
        }
    }
    None
}

/// Finds the directory that contains `suffix` as a relative path underneath it (e.g. a directory
/// `D` such that `D/ac3forge_c/export.h` exists), searching under `root`. Used to locate the
/// generated `export.h`'s own include root without assuming the exact `CMAKE_CURRENT_BINARY_DIR`
/// nesting `generate_export_header()` used.
fn find_dir_containing(root: &Path, suffix: &Path) -> Option<PathBuf> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        if dir.join(suffix).is_file() {
            return Some(dir);
        }
        let entries = match std::fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            }
        }
    }
    None
}
