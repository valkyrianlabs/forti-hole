# frozen_string_literal: true

# Formula template. `vlr homebrew formula` renders the release url/sha256 into
# release/homebrew/forti-hole.rb, which `vlr homebrew publish` commits to valkyrianlabs/homebrew-tap.
class FortiHole < Formula
  desc "DNS blocklists as FortiGate threat feeds, deduplicated, budgeted and verified"
  homepage "https://github.com/valkyrianlabs/forti-hole"
  url "https://github.com/valkyrianlabs/forti-hole/releases/download/v1.0.0/forti-hole-1.0.0.tar.gz"
  sha256 "TODO_REPLACE_WITH_RELEASE_ARCHIVE_SHA256"
  license "MIT"
  head "https://github.com/valkyrianlabs/forti-hole.git", branch: "main"

  # Header-only SDK: needed to compile, not at run time. Feed status and paging arrived in 1.3.0.
  depends_on "forti-api" => :build
  depends_on "meson" => :build
  depends_on "ninja" => :build
  depends_on "pkgconf" => :build

  uses_from_macos "curl"

  # The command engine, compiled in from forti-cli's source at exactly the revision
  # subprojects/forti-cli.wrap pins (packaging/forti-cli-pin.sh keeps the two in step). forti-hole
  # needs neither the forti-cli formula nor the `forti` executable at run time; profiles are
  # usually created with `forti configure`, so install valkyrianlabs/tap/forti-cli as well.
  resource "forti-cli" do
    url "https://github.com/valkyrianlabs/forti-cli/archive/13b7dfed99772ad20ff0e6744a5224cc63f38880.tar.gz"
    sha256 "455e413b307d57e7fc2de76d6e7dcf75f8d333b04c855128b4e0030c5ceed72f"
  end

  def install
    # Meson uses an existing subprojects/forti-cli as is, so nothing is fetched during the build.
    resource("forti-cli").stage buildpath/"subprojects/forti-cli"

    # std_meson_args sets --buildtype=release and --wrap-mode=nofallback: forti-api always comes from
    # the forti-api formula, never from subprojects/forti-api.wrap.
    # optimization=3 is meson.build's release profile, repeated so it holds whatever the build type;
    # strip=true installs the binary without its symbol table.
    # The tests need GoogleTest and run in CI; the `test do` block exercises the installed binary.
    # meson.build installs the systemd user units only on Linux, so a macOS install has none.
    system "meson", "setup", "build", "-Dtests=disabled", "-Doptimization=3", "-Dstrip=true", *std_meson_args
    system "meson", "compile", "-C", "build"
    system "meson", "install", "-C", "build", "--skip-subprojects"
  end

  def caveats
    <<~EOS
      Configure a device profile with `forti configure` (valkyrianlabs/tap/forti-cli), then:
        forti-hole setup          writes ~/.forti/hole/config.json ("enabled": false)
        forti-hole compile --device
        forti-hole plan
      Set "enabled": true, run `forti-hole sync`, and keep it current with
        brew services start forti-hole      (a sync twice a day)
    EOS
  end

  # `brew services start forti-hole`: one sync every 12 hours under launchd (macOS) or a systemd user
  # timer (Linuxbrew). A sync writes nothing until "enabled": true in ~/.forti/hole/config.json.
  service do
    run [opt_bin/"forti-hole", "sync", "--quiet"]
    run_type :interval
    interval 43200
    process_type :background
    log_path var/"log/forti-hole.log"
    error_log_path var/"log/forti-hole.log"
  end

  test do
    # Offline: neither needs a profile, a device or the network.
    ENV["FORTI_HOME"] = (testpath/"forti").to_s
    assert_match "forti-hole #{version} (engine forti-cli ", shell_output("#{bin}/forti-hole --version")
    help = shell_output("#{bin}/forti-hole --help")
    %w[setup doctor compile plan sync status attach remove import migrate].each do |command|
      assert_match command, help
    end
    system bin/"forti-hole", "setup", "--offline"
    assert_path_exists testpath/"forti/hole/config.json"
    assert_match "\"enabled\": false", (testpath/"forti/hole/config.json").read
  end
end
