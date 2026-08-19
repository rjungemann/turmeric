class Turmeric < Formula
  desc "A Lisp that compiles to C99 with typeclasses, algebraic effects, and borrow checking"
  homepage "https://turmeric-lang.com"
  license "MIT"
  head "https://github.com/rjungemann/turmeric.git", branch: "main"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                    *std_cmake_args
    system "cmake", "--build", "build", "-j"
    bin.install "build/tur"
    (share/"turmeric").install "stdlib"

    # Runs `tur completion zsh` / `tur completion bash` -- the default
    # shell_parameter_format passes the shell name as a positional argument,
    # which is exactly the CLI shape.
    generate_completions_from_executable(bin/"tur", "completion",
                                         shells: [:zsh, :bash])
  end

  test do
    # Exercises stdlib resolution: `when` is defined in stdlib/macros.tur,
    # so if the formula didn't install stdlib under <prefix>/share/turmeric/
    # this test fails with "unbound variable: when". Uses --interpret to skip
    # the C-codegen path, which currently still depends on src/runtime/*.c
    # being reachable (tracked separately in docs/release-binaries-plan.md).
    (testpath/"hello.tur").write <<~TUR
      (defn main [] :int
        (when (= 1 1) (println "hello from stdlib"))
        0)
    TUR
    assert_match "hello from stdlib",
                 shell_output("#{bin}/tur --interpret #{testpath}/hello.tur")
  end
end
