from siliconcompiler import Design

from switchboard import sb_path


class VerilatorSync(Design):
    """
    SiliconCompiler Design for cycle-synchronized Verilator testbench.

    This class provides the testbench_sync.cc file which includes barrier
    synchronization for cycle-accurate multi-process simulation.

    Unlike the standard Verilator testbench, this version:
    - Accepts barrier_uri, barrier_leader, barrier_procs plusargs
    - Synchronizes with other processes at each clock cycle
    - Enables true cycle-accurate simulation across process boundaries
    """
    def __init__(self):
        super().__init__("verilator_sync")

        self.set_dataroot('localroot', sb_path() / "verilator")

        with self.active_fileset('verilator'):
            self.add_file("testbench_sync.cc")
