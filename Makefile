# Host build: tests and measurements. Target build: compile check for the Model:Cycles.
#   make test       unit tests of the core (host)
#   make filter-test    unit tests of the filter (host); make filter-probe builds tools/filter_probe
#   make measure    quality table; add SAMPLES=build/samples_table.bin for the Model:Samples one
#   make sweep      rate table designs against each other (about a minute)
#   make target     compile the core for the ColdFire and list its size and undefined symbols
#   make coeffs     regenerate src/smp_coeffs.c from the design values below
#   make filter-table   regenerate src/smp_filter_tab.c (tools/gen_filter_table.py); check-filter-table compares
#   make render     build tools/render (listening tests, see its header)
#   make machine-test   host tests of the machine: bridge (machine/sampler.c) and sample memory (machine/pool.c),
#                   with the switches and without
#   make uploader-test  host tests of the upload page (uploader/, needs node)
# The machine itself: python3 machine/build.py --stock ... --modded-cycles ... (see README)

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
TCC     := m68k-elf-gcc
TFLAGS  := -std=c99 -mcpu=54418 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
           -Wall -Wextra
B       := build

# The table in use (tools/design.h): one least-squares prototype, passband to 0.34 cycles per
# source frame (16.3 kHz), stopband from 0.55 (26.4 kHz) weighted 100, no extra bump.
DESIGN  := proto 0.34 0.55 100 0

CORE    := src/smp_core.c src/smp_coeffs.c
FILTER  := src/smp_filter.c src/smp_filter_tab.c

.PHONY: test filter-test filter-probe measure sweep target coeffs check-coeffs filter-table check-filter-table \
        render machine-test uploader-test clean

$(B):
	mkdir -p $(B)

$(B)/test_core: tests/test_core.c $(CORE) src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -DSMP_DEBUG -o $@ tests/test_core.c $(CORE) -lm

$(B)/test_filter: tests/test_filter.c $(FILTER) src/smp_filter.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_filter.c $(FILTER) -lm

$(B)/filter_probe: tools/filter_probe.c $(FILTER) src/smp_filter.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/filter_probe.c $(FILTER) -lm

filter-probe: $(B)/filter_probe

$(B)/measure: tests/measure.c tools/design.c tools/design.h $(CORE) src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/measure.c tools/design.c $(CORE) -lm

$(B)/gen_coeffs: tools/gen_coeffs.c tools/design.c tools/design.h src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/gen_coeffs.c tools/design.c -lm

$(B)/render: tools/render.c tools/design.c tools/design.h $(CORE) src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/render.c tools/design.c $(CORE) -lm

render: $(B)/render

MACHINE := machine/sampler.c machine/pool.c machine/pool.h machine/view.c machine/view.h machine/os.h \
           machine/host_os.h

# with the switches (model27's builds, SMP_SWITCHES) and without them (One-Shot's own build)
$(B)/test_sampler: machine/test_sampler.c $(MACHINE) $(CORE) $(FILTER) src/smp_core.h src/smp_filter.h | $(B)
	$(CC) -std=c11 -O2 -Wall -Wextra -DSMP_DEBUG -DSMP_SWITCHES -Isrc -Imachine -o $@ machine/test_sampler.c $(CORE) \
	    $(FILTER) -lm

$(B)/test_sampler_plain: machine/test_sampler.c $(MACHINE) $(CORE) $(FILTER) src/smp_core.h src/smp_filter.h | $(B)
	$(CC) -std=c11 -O2 -Wall -Wextra -DSMP_DEBUG -Isrc -Imachine -o $@ machine/test_sampler.c $(CORE) $(FILTER) -lm

machine-test: $(B)/test_sampler $(B)/test_sampler_plain
	$(B)/test_sampler
	$(B)/test_sampler_plain

uploader-test:
	node uploader/test.js
	node uploader/test-app.js

test: $(B)/test_core
	$(B)/test_core

filter-test: $(B)/test_filter
	$(B)/test_filter

measure: $(B)/measure
	$(B)/measure $(if $(SAMPLES),--samples $(SAMPLES))

sweep: $(B)/measure
	$(B)/measure --sweep $(if $(SAMPLES),--samples $(SAMPLES))

coeffs: $(B)/gen_coeffs
	$(B)/gen_coeffs $(DESIGN) > src/smp_coeffs.c

check-coeffs: $(B)/gen_coeffs
	$(B)/gen_coeffs $(DESIGN) | cmp - src/smp_coeffs.c

filter-table:
	python3 tools/gen_filter_table.py > src/smp_filter_tab.c

check-filter-table:
	python3 tools/gen_filter_table.py | cmp - src/smp_filter_tab.c

target: | $(B)
	$(TCC) $(TFLAGS) -c -o $(B)/smp_core.o src/smp_core.c
	$(TCC) $(TFLAGS) -c -o $(B)/smp_coeffs.o src/smp_coeffs.c
	$(TCC) $(TFLAGS) -c -o $(B)/smp_filter.o src/smp_filter.c
	$(TCC) $(TFLAGS) -c -o $(B)/smp_filter_tab.o src/smp_filter_tab.c
	$(TCC) $(TFLAGS) -c -o $(B)/smp_filter_cf.o src/smp_filter_cf.S
	m68k-elf-ld -r -o $(B)/sampler.o $(B)/smp_core.o $(B)/smp_coeffs.o $(B)/smp_filter.o $(B)/smp_filter_tab.o \
	    $(B)/smp_filter_cf.o
	m68k-elf-size $(B)/smp_core.o $(B)/smp_coeffs.o $(B)/smp_filter.o $(B)/smp_filter_tab.o $(B)/smp_filter_cf.o
	@echo "undefined symbols (must be none):"
	@m68k-elf-nm -u $(B)/sampler.o

clean:
	rm -rf $(B)
