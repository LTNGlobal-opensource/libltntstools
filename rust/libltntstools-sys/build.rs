use anyhow::{ensure, Context, Result as Fallible};
use bindgen::callbacks::ParseCallbacks;
use std::{
    env,
    ffi::{OsStr, OsString},
    fs,
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Debug)]
struct Callbacks {
    out_dir: PathBuf,
}

impl Callbacks {
    fn new(out_dir: &Path) -> Self {
        Self {
            out_dir: out_dir.into(),
        }
    }
}

impl ParseCallbacks for Callbacks {
    fn item_name(&self, original_item_name: &str) -> Option<String> {
        if let Some(name) = original_item_name.strip_prefix("ltntstools_") {
            return Some(name.into());
        }

        if let Some(name) = original_item_name.strip_prefix("libltntstools_") {
            return Some(name.into());
        }

        None
    }

    fn process_comment(&self, comment: &str) -> Option<String> {
        let comment = doxygen_rs::generator::rustdoc(comment.into()).unwrap_or_else(|e| {
            println!("cargo::warning=Error transforming comment: {e:?}");
            format!("(doxygen parsing failed)\n\n```doxygen\n{comment}\n```")
        });
        Some(comment)
    }

    fn include_file(&self, filename: &str) {
        let filename = Path::new(filename).canonicalize().unwrap();
        if !filename.starts_with(&self.out_dir) {
            println!("cargo:rerun-if-changed={}", filename.display());
        }
    }

    fn read_env_var(&self, key: &str) {
        println!("cargo:rerun-if-env-changed={}", key);
    }
}

fn prepend_pkg_config_path(path: &Path) -> Fallible<()> {
    let pc_path = env::var_os("PKG_CONFIG_PATH")
        .map(|s| env::join_paths(Some(path.into()).into_iter().chain(env::split_paths(&s))))
        .transpose()
        .context("Bad PKG_CONFIG_PATH")?
        .unwrap_or_else(|| path.into());

    // SAFETY: This is only safe in a single-threaded program.
    unsafe { env::set_var("PKG_CONFIG_PATH", pc_path) };
    Ok(())
}

fn set_default_cflags(include_dir: &Path) {
    // cc's compiler arguments already include CFLAGS from the environment
    let compiler = cc::Build::new().warnings(false).get_compiler();
    let mut cflags = compiler.args().to_vec().join(OsStr::new(" "));

    cflags.push(" -I");
    cflags.push(include_dir);

    // SAFETY: This is only safe in a single-threaded program.
    unsafe { env::set_var("CFLAGS", cflags) };
}

fn set_default_ldflags(lib_dir: &Path) {
    let mut ldflags = env::var_os("LDFLAGS").unwrap_or_default();

    ldflags.push(" -L");
    ldflags.push(lib_dir);

    // SAFETY: This is only safe in a single-threaded program.
    unsafe { env::set_var("LDFLAGS", ldflags) };
}

fn run_autoreconf(dir: &Path) -> Fallible<()> {
    if dir.join("configure").exists() {
        println!(
            "{} is already bootstrapped, skipping autoreconf",
            dir.display(),
        );
        return Ok(());
    }

    let status = Command::new("autoreconf")
        .arg("-fvi")
        .current_dir(dir)
        .status()
        .with_context(|| format!("Failed to autoreconf {}", dir.display()))?;

    ensure!(
        status.success(),
        "Failed to autoreconf in {}; exit status {}",
        dir.display(),
        status,
    );
    Ok(())
}

fn run_configure<F>(srcdir: &Path, builddir: &Path, f: F) -> Fallible<()>
where
    F: FnOnce(&mut Command) -> Fallible<()>,
{
    if builddir.join("Makefile").exists() {
        println!(
            "{} is already configured, skipping configure",
            builddir.display(),
        );
        return Ok(());
    }

    fs::create_dir_all(builddir)
        .with_context(|| format!("Failed to create {}", builddir.display()))?;

    let mut configure = Command::new(srcdir.join("configure"));
    configure.current_dir(builddir);

    f(&mut configure)?;

    let status = configure.status().with_context(|| {
        format!(
            "Failed to configure {} in {}",
            srcdir.display(),
            builddir.display()
        )
    })?;

    ensure!(
        status.success(),
        "Failed to configure {} in {}; exit status {}",
        srcdir.display(),
        builddir.display(),
        status,
    );
    Ok(())
}

fn run_make(dir: &Path, target: &str) -> Fallible<()> {
    let mut make = Command::new("make");

    if let Ok(jobs) = env::var("NUM_JOBS") {
        make.env("MAKEFLAGS", &format!("-j{jobs}"));
    }

    let status = make
        .arg(target)
        .current_dir(dir)
        .status()
        .with_context(|| format!("Failed to make {} in {}", target, dir.display()))?;

    ensure!(
        status.success(),
        "Failed to make {} in {}; exit status {}",
        target,
        dir.display(),
        status,
    );
    Ok(())
}

fn create_canonical_dir(path: &Path) -> Fallible<PathBuf> {
    fs::create_dir_all(path)?;
    Ok(path.canonicalize()?)
}

fn main() -> Fallible<()> {
    let out_dir: PathBuf = env::var_os("OUT_DIR").context("OUT_DIR is missing")?.into();
    let include_dir = create_canonical_dir(&out_dir.join("include"))?;
    let lib_dir = create_canonical_dir(&out_dir.join("lib"))?;

    set_default_cflags(&include_dir);
    set_default_ldflags(&lib_dir);

    let srcdir = env::current_dir()?.canonicalize()?;
    let builddir = create_canonical_dir(&out_dir.join("build"))?;

    let libdvbpsi_srcdir = srcdir.join("libdvbpsi");
    run_autoreconf(&libdvbpsi_srcdir)?;

    let libdvbpsi_builddir = builddir.join("libdvbpsi");
    run_configure(&libdvbpsi_srcdir, &libdvbpsi_builddir, |configure| {
        configure
            .arg("--prefix")
            .arg(&out_dir)
            .args(["--enable-static", "--disable-shared"]);
        Ok(())
    })?;
    run_make(&libdvbpsi_builddir, "install")?;

    let ffmpeg_srcdir = srcdir.join("ffmpeg");
    let ffmpeg_builddir = builddir.join("ffmpeg");
    run_configure(&ffmpeg_srcdir, &ffmpeg_builddir, |configure| {
        let mut prefix_arg = OsString::new();
        prefix_arg.push("--prefix=");
        prefix_arg.push(&out_dir);

        configure
            .arg(prefix_arg)
            .args(["--enable-static", "--disable-shared"])
            .arg("--disable-programs")
            .arg("--disable-iconv")
            .args([
                "--disable-audiotoolbox",
                "--disable-videotoolbox",
                "--disable-avfoundation",
            ])
            .args(["--disable-vaapi", "--disable-vdpau"])
            .arg("--pkg-config-flags=--static");
        Ok(())
    })?;
    run_make(&ffmpeg_builddir, "install")?;

    let libltntstools_srcdir = srcdir.join("../..");
    let libltntstools_builddir = builddir.join("libltntstools");
    run_autoreconf(&libltntstools_srcdir)?;
    run_configure(
        &libltntstools_srcdir,
        &libltntstools_builddir,
        |configure| {
            configure
                .arg("--prefix")
                .arg(&out_dir)
                .args(["--enable-static", "--disable-shared"])
                .env("CFLAGS", {
                    let mut cflags = env::var_os("CFLAGS").unwrap_or_default();
                    cflags.push(" -I");
                    cflags.push(&ffmpeg_srcdir);
                    cflags
                });
            Ok(())
        },
    )?;
    run_make(&libltntstools_builddir, "install")?;

    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_arg("-I")
        .clang_arg(include_dir.to_string_lossy())
        .ctypes_prefix("libc")
        .allowlist_file(include_dir.join("libltntstools/.*").to_string_lossy())
        .blocklist_type("_IO_.*")
        .blocklist_type("__(suseconds|time)_t")
        .blocklist_type("__off(64)?_t")
        .blocklist_type("__pthread_.*")
        .blocklist_type("__u(_.*|.*_t)")
        .blocklist_type("(bpf|pcap)_.*")
        .blocklist_type("FILE")
        .blocklist_type("in_.*")
        .blocklist_type("pthread_.*")
        .blocklist_type("sa_.*")
        .blocklist_type("sockaddr_in")
        .blocklist_type("time(_t|val)")
        .blocklist_type("u_.*")
        .raw_line("type u_char = u8; type u_int32_t = u32;")
        .raw_line("use libc::FILE;")
        .raw_line("use libc::sockaddr_in;")
        .raw_line("use libc::{pthread_cond_t, pthread_mutex_t, pthread_t};")
        .raw_line("use libc::{time_t, timeval};")
        .raw_line("use pcap::PacketHeader as pcap_pkthdr;")
        .default_enum_style(bindgen::EnumVariation::NewType {
            is_bitfield: false,
            is_global: false,
        })
        .derive_default(true)
        .impl_debug(true)
        .parse_callbacks(Box::new(Callbacks::new(&out_dir)))
        .generate()
        .context("Error generating bindings")?;

    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .context("Error writing bindings")?;

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=ltntstools");

    prepend_pkg_config_path(&lib_dir.join("pkgconfig"))?;
    pkg_config::probe_library("libavformat")?;
    pkg_config::probe_library("libdvbpsi")?;

    Ok(())
}
