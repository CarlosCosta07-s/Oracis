//! Generates the Oracis icon as a standalone SVG asset.
//!
//! The Rust source is the single source of truth. Run from the repository root:
//! `cargo run --manifest-path tools/oracis-icongen/Cargo.toml`

use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::ExitCode,
};

const ICON_SVG: &str = r##"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">
  <title>Oracis</title>
  <defs>
    <linearGradient id="background" x1="0" y1="0" x2="1024" y2="1024" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#3B315F"/><stop offset="0.52" stop-color="#202548"/><stop offset="1" stop-color="#11172E"/>
    </linearGradient>
    <radialGradient id="aura" cx="512" cy="508" r="420" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#80C8E8" stop-opacity="0.38"/><stop offset="0.58" stop-color="#547EBD" stop-opacity="0.13"/><stop offset="1" stop-color="#547EBD" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="orbit" x1="220" y1="330" x2="825" y2="700" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#FFF0B8"/><stop offset="0.28" stop-color="#D9B86B"/><stop offset="0.62" stop-color="#F8E9B7"/><stop offset="1" stop-color="#A87536"/>
    </linearGradient>
    <linearGradient id="orbitShade" x1="280" y1="300" x2="760" y2="760" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#8D633B"/><stop offset="1" stop-color="#44304D"/>
    </linearGradient>
    <radialGradient id="platinum" cx="420" cy="390" r="480" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#FFFFFF"/><stop offset="0.34" stop-color="#E9F2F8"/><stop offset="0.72" stop-color="#AFC3D8"/><stop offset="1" stop-color="#647A9B"/>
    </radialGradient>
    <linearGradient id="coreFacet" x1="350" y1="330" x2="690" y2="690" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.92"/><stop offset="0.48" stop-color="#BBD2E8" stop-opacity="0.28"/><stop offset="1" stop-color="#526B8D" stop-opacity="0.78"/>
    </linearGradient>
    <linearGradient id="goldCore" x1="430" y1="430" x2="610" y2="610" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#FFF5CA"/><stop offset="0.48" stop-color="#E8C776"/><stop offset="1" stop-color="#A66F34"/>
    </linearGradient>
    <radialGradient id="jewel" cx="485" cy="470" r="115" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#E9FFFF"/><stop offset="0.4" stop-color="#8DDCEB"/><stop offset="1" stop-color="#397FAD"/>
    </radialGradient>
    <linearGradient id="glint" x1="350" y1="350" x2="520" y2="520" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.72"/><stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>
    </linearGradient>
  </defs>
  <rect width="1024" height="1024" rx="224" fill="url(#background)"/>
  <circle cx="512" cy="508" r="420" fill="url(#aura)"/>
  <circle cx="512" cy="512" r="365" fill="none" stroke="#B8CBE0" stroke-opacity="0.10" stroke-width="3"/>
  <circle cx="512" cy="512" r="315" fill="none" stroke="#E8D39A" stroke-opacity="0.12" stroke-width="2" stroke-dasharray="5 22"/>
  <path d="M512 142 V198 M512 826 V882 M142 512 H198 M826 512 H882" stroke="#D6E6F4" stroke-opacity="0.26" stroke-width="8" stroke-linecap="round"/>
  <ellipse cx="512" cy="866" rx="235" ry="34" fill="#030817" fill-opacity="0.36"/>
  <!-- The ring of creation: an original orbital motif, not a character silhouette. -->
  <ellipse cx="512" cy="510" rx="342" ry="211" transform="rotate(-31 512 510)" fill="none" stroke="url(#orbitShade)" stroke-width="58" stroke-linecap="round" opacity="0.72"/>
  <ellipse cx="512" cy="500" rx="342" ry="211" transform="rotate(-31 512 500)" fill="none" stroke="url(#orbit)" stroke-width="39" stroke-linecap="round"/>
  <path d="M245 359 C302 292 386 251 473 241" fill="none" stroke="#FFF8D9" stroke-opacity="0.65" stroke-width="9" stroke-linecap="round"/>
  <path d="M746 650 C690 708 616 745 544 758" fill="none" stroke="#8D633B" stroke-opacity="0.55" stroke-width="10" stroke-linecap="round"/>
  <circle cx="246" cy="357" r="31" fill="#714F40" opacity="0.48"/><circle cx="246" cy="348" r="25" fill="url(#goldCore)"/><circle cx="246" cy="340" r="8" fill="#FFFFFF" fill-opacity="0.68"/>
  <circle cx="778" cy="650" r="31" fill="#714F40" opacity="0.48"/><circle cx="778" cy="641" r="25" fill="url(#goldCore)"/><circle cx="778" cy="633" r="8" fill="#FFFFFF" fill-opacity="0.58"/>
  <!-- Faceted platinum star: a symbol of origin, balance and celestial order. -->
  <path d="M512 213 L579 424 L790 512 L579 600 L512 811 L445 600 L234 512 L445 424 Z" transform="translate(0 13)" fill="#030817" fill-opacity="0.32"/>
  <path d="M512 213 L579 424 L790 512 L579 600 L512 811 L445 600 L234 512 L445 424 Z" fill="#536984"/>
  <path d="M512 232 L572 432 L770 512 L572 592 L512 792 L452 592 L254 512 L452 432 Z" fill="url(#platinum)"/>
  <path d="M512 232 L572 432 L512 512 L452 432 Z" fill="#FFFFFF" fill-opacity="0.52"/>
  <path d="M770 512 L572 592 L512 512 L572 432 Z" fill="#6B82A1" fill-opacity="0.42"/>
  <path d="M512 792 L452 592 L512 512 L572 592 Z" fill="#536B8B" fill-opacity="0.38"/>
  <path d="M254 512 L452 432 L512 512 L452 592 Z" fill="#FFFFFF" fill-opacity="0.24"/>
  <path d="M512 232 L512 512 L770 512 L572 432 Z" fill="url(#coreFacet)" opacity="0.45"/>
  <path d="M512 388 L551 473 L636 512 L551 551 L512 636 L473 551 L388 512 L473 473 Z" fill="#80613F" fill-opacity="0.34" transform="translate(0 9)"/>
  <path d="M512 388 L551 473 L636 512 L551 551 L512 636 L473 551 L388 512 L473 473 Z" fill="url(#goldCore)" stroke="#FFF1C2" stroke-opacity="0.72" stroke-width="5"/>
  <path d="M512 420 L538 486 L604 512 L538 538 L512 604 L486 538 L420 512 L486 486 Z" fill="url(#jewel)" stroke="#F6F1D6" stroke-opacity="0.82" stroke-width="5"/>
  <path d="M512 420 L512 512 L604 512 L538 486 Z" fill="#FFFFFF" fill-opacity="0.28"/>
  <ellipse cx="390" cy="388" rx="45" ry="93" transform="rotate(-34 390 388)" fill="url(#glint)"/>
  <circle cx="438" cy="338" r="16" fill="#FFFFFF" fill-opacity="0.72"/>
  <path d="M696 290 L709 324 L743 337 L709 350 L696 384 L683 350 L649 337 L683 324 Z" fill="#E9F4FF" fill-opacity="0.70"/>
  <circle cx="332" cy="700" r="9" fill="#E8D39A" fill-opacity="0.78"/><circle cx="700" cy="760" r="7" fill="#B7E8F1" fill-opacity="0.72"/>
</svg>"##;

fn repository_root() -> PathBuf {
    env::args_os().nth(1).map(PathBuf::from).unwrap_or_else(|| {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(2)
            .map_or_else(|| PathBuf::from("."), Path::to_path_buf)
    })
}

fn generate(root: &Path) -> io::Result<()> {
    let output = root.join("assets/icon/oracis.svg");
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&output, ICON_SVG.as_bytes())?;
    println!("generated {}", output.display());
    Ok(())
}

fn main() -> ExitCode {
    match generate(&repository_root()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: could not generate Oracis icon: {error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn icon_is_a_square_vector_asset() {
        assert!(ICON_SVG.contains("viewBox=\\\"0 0 1024 1024\\\""));
        assert!(ICON_SVG.contains("<title>Oracis</title>"));
    }

    #[test]
    fn icon_uses_the_project_palette_and_creation_sigil() {
        assert!(ICON_SVG.contains("#3B315F"));
        assert!(ICON_SVG.contains("#FFF0B8"));
        assert!(ICON_SVG.contains("Faceted platinum star"));
    }
}
