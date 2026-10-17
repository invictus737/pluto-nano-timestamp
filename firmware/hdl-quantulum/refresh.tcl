# Refresh the packaged checksums of the vendored timestamp IP cores with the Vivado in use
# (component.xml was edited by hand: ADI hdl 2023_R2 removed SYNC from the fifo_wr interface).
# usage: vivado -mode batch -source refresh.tcl -tclargs <hdl-quantulum dir>
set root [file normalize [lindex $argv 0]]
foreach xml [glob -nocomplain [file join $root * component.xml]] {
  set core [ipx::open_core -set_current false $xml]
  ipx::update_checksums $core
  ipx::save_core $core
  ipx::unload_core $core
  puts "refreshed $xml"
}
