from siliconcompiler import Design

from switchboard import sb_path


class BarrierDPI(Design):
    """
    SiliconCompiler Design for barrier synchronization DPI-C interface.

    This class provides the barrier_dpi.cc file which implements the
    DPI-C bridge between Verilog and the C++ barrier synchronization
    code in barrier_sync.h.

    The DPI functions provided are:
    - barrier_open(): Open/create a shared memory barrier
    - barrier_wait(): Wait at barrier (blocking) - returns cycle count
    - barrier_close(): Clean up barrier resources
    """
    def __init__(self):
        super().__init__("barrier_dpi")

        self.set_dataroot('localroot', sb_path() / "dpi")

        with self.active_fileset('sim'):
            self.add_file("barrier_dpi.cc")
