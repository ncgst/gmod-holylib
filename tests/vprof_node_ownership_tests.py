"""Exercise the production VProf GC body against independently owned nodes."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (args.source or root / "source/modules/vprof.cpp").read_text(encoding="utf-8")
body = re.search(r"Default__gc\(CVProfNode,\s*(.*?)\n\)", source, re.S).group(1)
program = r'''
#include <cassert>
static int destroyed = 0;
struct CVProfNode {
    int value = 42;
    ~CVProfNode() { ++destroyed; }
};
struct Wrapper { void* data; };
void collect(Wrapper& wrapper) {
    [[maybe_unused]] void* pStoredData = wrapper.data;
    wrapper.data = nullptr;
    // PRODUCTION_GC_BODY
}
int main() {
    auto* engineNode = new CVProfNode;
    Wrapper first{engineNode}, alias{engineNode};
    collect(first);
    assert(first.data == nullptr);
    assert(destroyed == 0);
    assert(static_cast<CVProfNode*>(alias.data)->value == 42);
    collect(first);
    collect(alias);
    assert(destroyed == 0);
    delete engineNode;
    assert(destroyed == 1);
}
'''.replace("// PRODUCTION_GC_BODY", body)
with tempfile.TemporaryDirectory(prefix="vprof-ownership-") as temp:
    cpp, binary = Path(temp) / "test.cpp", Path(temp) / "test"
    cpp.write_text(program, encoding="utf-8")
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("VProf borrowed-node ownership regression passed")
