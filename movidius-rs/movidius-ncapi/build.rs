// Build script to generate C headers
use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let output_file = PathBuf::from(&crate_dir)
        .join("..")
        .join("include")
        .join("movidius_ncapi.h");

    std::fs::create_dir_all(output_file.parent().unwrap()).ok();

    cbindgen::Builder::new()
        .with_crate(crate_dir)
        .with_language(cbindgen::Language::C)
        .with_style(cbindgen::Style::Both)
        .with_documentation(true)
        .generate()
        .expect("Unable to generate C bindings")
        .write_to_file(&output_file);

    println!("cargo:rerun-if-changed=src/");
}
