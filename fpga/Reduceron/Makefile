SRC=toplevel.v \
    ../../york-lava/simulation/altsyncram.v \
    ../../york-lava/simulation/lpm_add_sub.v

sim: obj_dir/Vtoplevel
	@for x in *.mif;do grep : < $$x|sed -e "s,^.*:,," -e "s,;,," > $$x.txt;done
	@./obj_dir/Vtoplevel

obj_dir/Vtoplevel: $(SRC)
	@verilator --cc toplevel.v ../../york-lava/simulation/altsyncram.v --exe sim_main.cpp 2>&1 > /dev/null
	@make -C obj_dir -j -f Vtoplevel.mk Vtoplevel 2>&1 > /dev/null
