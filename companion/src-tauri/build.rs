//! Compiles the vendored MediaRemote adapter (macOS only), then runs Tauri's build step.

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        build_media_adapter();
    }
    tauri_build::build()
}

/// Builds `resources/mediaremote/MediaRemoteAdapter.framework` and copies the Perl script next to it.
/// The framework is a flat bundle (no Versions/ symlinks): the script only loads
/// `<framework>/MediaRemoteAdapter`, and symlinks do not survive resource copying.
fn build_media_adapter() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let vendor = manifest.join("../vendor/mediaremote-adapter");
    let out = manifest.join("resources/mediaremote");
    let framework = out.join("MediaRemoteAdapter.framework");
    println!("cargo:rerun-if-changed={}", vendor.display());

    let sources: Vec<PathBuf> = ["src/adapter", "src/private", "src/utility"]
        .iter()
        .flat_map(|dir| std::fs::read_dir(vendor.join(dir)).unwrap())
        .map(|entry| entry.unwrap().path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "m"))
        .collect();

    std::fs::create_dir_all(framework.join("Resources")).unwrap();
    let binary = framework.join("MediaRemoteAdapter");
    let status = Command::new("xcrun")
        .args(["clang", "-dynamiclib", "-fobjc-arc", "-fvisibility=default", "-O2"])
        .args(["-arch", "arm64", "-arch", "x86_64", "-mmacosx-version-min=12.0"])
        .arg("-I").arg(vendor.join("include"))
        .arg("-I").arg(vendor.join("src"))
        .args(["-framework", "Foundation", "-framework", "AppKit", "-framework", "UniformTypeIdentifiers"])
        .args(["-install_name", "@rpath/MediaRemoteAdapter.framework/MediaRemoteAdapter"])
        .args(&sources)
        .arg("-o").arg(&binary)
        .status()
        .expect("xcrun clang is needed to build the media adapter (Xcode command line tools)");
    assert!(status.success(), "building MediaRemoteAdapter failed");

    write_info_plist(&framework.join("Resources/Info.plist"));
    std::fs::copy(vendor.join("bin/mediaremote-adapter.pl"), out.join("mediaremote-adapter.pl")).unwrap();
    // Ad-hoc signature, as the upstream build does: unsigned code may not be loaded
    let _ = Command::new("codesign").args(["--force", "--sign", "-"]).arg(&binary).status();
}

fn write_info_plist(path: &Path) {
    let plist = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>MediaRemoteAdapter</string>
  <key>CFBundleIdentifier</key><string>com.vandenbe.MediaRemoteAdapter</string>
  <key>CFBundleName</key><string>MediaRemoteAdapter</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>0.1.0</string>
</dict>
</plist>
"#;
    std::fs::write(path, plist).unwrap();
}
