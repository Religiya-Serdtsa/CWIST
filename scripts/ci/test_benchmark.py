"""Regression tests for the report CLI; no build or benchmark dependencies."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("benchmark.py")
PROFILE = "wrk -t12 -c400 -d10s (after 10s warmup, warmup discarded)"
OPTIONS = (
    "-Xms1024m -Xmx1024m   -XX:+UseG1GC\t"
    "-Xlog:gc*:file=/tmp/spring_gc.log:time,uptime,level,tags "
    "-XX:AOTCache=/tmp/spring_bench/app.aot (JEP 483 + JEP 514 single-step AOT)"
)
ENV = {
    "java_version": 'openjdk version "25.0.4.1" 2026-08-18 LTS',
    "spring_boot_version": "3.2.3",
    "stack": "Spring WebFlux + Reactor Netty on native epoll (G1GC, JDK 25 Leyden AOT)",
    "jvm_opts": OPTIONS,
    "virtual_threads": False,
}
START = "<!-- WEBSERVER_BENCHMARKS:START -->"
END = "<!-- WEBSERVER_BENCHMARKS:END -->"


class BenchmarkRenderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        script = self.root / "scripts/ci/benchmark.py"
        script.parent.mkdir(parents=True)
        shutil.copyfile(SCRIPT, script)
        (self.root / "benchmarks").mkdir()
        (self.root / "benchmarks/db.json").write_text("[]\n")
        block = f"{START}\nold report\n{END}\n"
        (self.root / "README").write_text(
            "<!-- BENCHMARKS:START -->\nold\n<!-- BENCHMARKS:END -->\n" + block
        )
        (self.root / "README.md").write_text("# Before\n\n" + block + "\n# After\n")
        (self.root / "ROADMAP.md").write_text(
            "<!-- CI-BENCHMARKS:START -->\nold\n<!-- CI-BENCHMARKS:END -->\n"
        )

    def render(self, env=ENV):
        row = {"cwist_rps": 12345, "spring_rps": 6789, "wrk_profile": PROFILE}
        if env is not None:
            row["spring_env"] = env
        history = self.root / "benchmarks/webserver.json"
        history.write_text(json.dumps([row]) + "\n")
        before = history.read_bytes()
        subprocess.run(
            [sys.executable, str(self.root / "scripts/ci/benchmark.py"), "render"],
            check=True, capture_output=True, text=True,
        )
        self.assertEqual(history.read_bytes(), before)
        readme = (self.root / "README.md").read_text()
        plain = (self.root / "README").read_text()
        self.assertEqual(self.block(readme), self.block(plain))
        self.assertTrue(readme.startswith("# Before\n\n" + START))
        self.assertTrue(readme.endswith(END + "\n\n# After\n"))
        self.assertIn("**CWIST (classic pool)**: 12345 req/s", readme)
        self.assertIn("**Spring Boot**: 6789 req/s", readme)
        return readme

    @staticmethod
    def block(text):
        return text.split(START, 1)[1].split(END, 1)[0]

    def test_runtime_fields_and_profile_are_separate_markdown_sections(self):
        readme = self.render()
        self.assertIn("\n**Spring runtime environment**\n\n", readme)
        self.assertIn(f"- **JDK:** `{ENV['java_version']}`\n", readme)
        self.assertIn("- **Spring Boot:** 3.2.3\n", readme)
        self.assertIn(f"- **Stack:** {ENV['stack']}\n", readme)
        self.assertIn(f"\n**Warmup/profile**\n\n{PROFILE}\n", readme)
        self.assertNotIn("Spring runtime env:", readme)

    def test_jvm_options_have_individual_lines_without_losing_annotation(self):
        readme = self.render()
        expected = (
            "\n**JVM options**\n\n```text\n"
            "-Xms1024m\n-Xmx1024m\n-XX:+UseG1GC\n"
            "-Xlog:gc*:file=/tmp/spring_gc.log:time,uptime,level,tags\n"
            "-XX:AOTCache=/tmp/spring_bench/app.aot (JEP 483 + JEP 514 single-step AOT)\n"
            "```\n"
        )
        self.assertIn(expected, readme)

    def test_quoted_option_values_remain_intact(self):
        options = '-Dlabel="value -with spaces"  -Dother=\'keep -this too\' -Xmx1024m'
        readme = self.render({**ENV, "jvm_opts": options})
        self.assertIn(
            '```text\n-Dlabel="value -with spaces"\n-Dother=\'keep -this too\'\n-Xmx1024m\n```',
            readme,
        )

    def test_virtual_threads_false_is_explicit(self):
        self.assertIn("- **Virtual threads:** disabled\n", self.render())

    def test_virtual_threads_true_is_explicit(self):
        readme = self.render({**ENV, "virtual_threads": True})
        self.assertIn("- **Virtual threads:** enabled\n", readme)

    def test_missing_optional_fields_do_not_invent_runtime_settings(self):
        readme = self.render({"java_version": "test JDK"})
        self.assertIn("- **Spring Boot:** n/a\n", readme)
        self.assertIn("```text\nn/a\n```", readme)
        self.assertNotIn("- **Stack:**", readme)
        self.assertNotIn("- **Virtual threads:**", readme)

    def test_no_environment_omits_runtime_section(self):
        for env in (None, {}):
            with self.subTest(env=env):
                self.assertNotIn("**Spring runtime environment**", self.render(env))

    def test_repeated_render_is_byte_identical(self):
        self.render()
        outputs = ["README", "README.md", "ROADMAP.md", "docs/benchmark-trends.svg",
                   "docs/webserver-benchmark-trends.svg"]
        first = {name: (self.root / name).read_bytes() for name in outputs}
        self.render()
        self.assertEqual(first, {name: (self.root / name).read_bytes() for name in outputs})


if __name__ == "__main__":
    unittest.main()
