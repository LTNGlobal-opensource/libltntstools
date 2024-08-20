use anyhow::{ensure, Context, Result as Fallible};
use bindgen::callbacks::ParseCallbacks;
use std::{env, path::PathBuf};

#[derive(Debug)]
struct Callbacks;

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
}

fn main() -> Fallible<()> {
    let out_dir: PathBuf = env::var_os("OUT_DIR").context("OUT_DIR is missing")?.into();

    let target_root = env::current_dir()
        .context("Failed to get current directory")?
        .join("../../../target-root")
        .canonicalize()
        .context("Failed to find target root")?;
    ensure!(target_root.is_dir(), "{target_root:?} is not a directory");

    let include_dir = target_root
        .join("usr/include")
        .canonicalize()
        .context("Target root has no include dir")?;

    let lib_dir = target_root
        .join("usr/lib")
        .canonicalize()
        .context("Target root has no lib dir")?;

    let pc_dir = lib_dir
        .join("pkgconfig")
        .canonicalize()
        .context("Target root has no pkgconfig dir")?;

    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_arg("-I")
        .clang_arg(include_dir.to_string_lossy())
        .ctypes_prefix("libc")
        .allowlist_file(include_dir.join("libltntstools/.*").to_string_lossy())
        .blocklist_type("_.*")
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
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .parse_callbacks(Box::new(Callbacks))
        .generate()
        .context("Error generating bindings")?;

    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .context("Error writing bindings")?;

    println!(
        "cargo:rustc-link-search=native={}",
        lib_dir.to_string_lossy(),
    );
    println!("cargo:rustc-link-lib=ltntstools");

    let pc_path = env::var_os("PKG_CONFIG_PATH")
        .map(|s| env::join_paths(Some(pc_dir.clone()).into_iter().chain(env::split_paths(&s))))
        .transpose()?
        .unwrap_or_else(|| pc_dir.into());
    unsafe { env::set_var("PKG_CONFIG_PATH", pc_path) };

    pkg_config::probe_library("libdvbpsi")?;
    pkg_config::probe_library("libavformat")?;

    Ok(())
}
