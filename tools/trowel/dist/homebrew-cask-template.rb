# Homebrew cask template for Trowel.
#
# This is a TEMPLATE -- it is not auto-published. To ship a cask:
#   1. Use the `homebrew-turmeric` tap repo under the project's GitHub org.
#   2. Drop a populated copy of this file at Casks/trowel.rb.
#   3. After each release, run `brew bump-cask-pr` (or hand-edit version +
#      sha256) and push.
#   4. Users install via: `brew tap <org>/turmeric && brew install --cask trowel`.
#
# Replace @TROWEL_VERSION@ and @SHA256@ at release time.

cask "trowel" do
  version "@TROWEL_VERSION@"
  sha256  "@SHA256@"

  arch arm: "arm64", intel: "x86_64"

  url "https://github.com/rjungemann/turmeric/releases/download/v#{version}/Trowel-#{version}-macos-#{arch}.dmg"
  name "Trowel"
  desc "Native code editor and REPL for the Turmeric programming language"
  homepage "https://github.com/rjungemann/turmeric"

  # Ship the editor; rely on the `turmeric` formula for the compiler.
  depends_on formula: "turmeric"
  depends_on macos: ">= :big_sur"

  app "Trowel.app"
  binary "#{appdir}/Trowel.app/Contents/MacOS/trowel-cli", target: "trowel"

  zap trash: [
    "~/.config/trowel",
    "~/Library/Saved Application State/com.trowel.editor.savedState",
  ]
end
