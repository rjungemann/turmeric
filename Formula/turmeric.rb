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
  end

  test do
    (testpath/"hello.tur").write '(println "hello")'
    assert_match "hello", shell_output("#{bin}/tur run hello.tur")
  end
end
