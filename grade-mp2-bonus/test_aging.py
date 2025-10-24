import sys, os
sys.path.append(os.path.dirname(os.path.dirname(__file__)))
from gradelib import *

r = Runner(save("xv6.out"))

@test(5, "bonus: aging (long-waited process eventually runs)")
def test_aging():
    r.run_qemu(shell_script([
        'bonus_aging',
    ]))
    r.match('AGING_PASS')
    
if __name__ == "__main__":
    run_tests()