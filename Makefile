# Host build: tests and measurements. Target build: compile check for the Model:Cycles.
#   make test       unit tests of the core (host)
#   make measure    quality table; add SAMPLES=build/samples_table.bin for the Model:Samples one
#   make sweep      rate table designs against each other (about a minute)
#   make target     compile the core for the ColdFire and list its size and undefined symbols
#   make coeffs     regenerate src/smp_coeffs.c from the design values below
#   make render     build tools/render (listening tests, see its header)
#   make machine-test   host tests of the machine: bridge (machine/sampler.c) and sample memory (machine/pool.c)
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

.PHONY: test measure sweep target coeffs check-coeffs render machine-test clean

$(B):
	mkdir -p $(B)

$(B)/test_core: tests/test_core.c $(CORE) src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -DSMP_DEBUG -o $@ tests/test_core.c $(CORE)

$(B)/measure: tests/measure.c tools/design.c tools/design.h $(CORE) src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -o $@ tests/measure.c tools/design.c $(CORE) -lm

$(B)/gen_coeffs: tools/gen_coeffs.c tools/design.c tools/design.h src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/gen_coeffs.c tools/design.c -lm

$(B)/render: tools/render.c tools/design.c tools/design.h $(CORE) src/smp_core.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/render.c tools/design.c $(CORE) -lm

render: $(B)/render

MACHINE := machine/sampler.c machine/pool.c machine/pool.h machine/os.h machine/host_os.h

$(B)/test_sampler: machine/test_sampler.c $(MACHINE) $(CORE) src/smp_core.h | $(B)
	$(CC) -std=c11 -O2 -Wall -Wextra -DSMP_DEBUG -Isrc -Imachine -o $@ machine/test_sampler.c $(CORE) -lm

machine-test: $(B)/test_sampler
	$(B)/test_sampler

test: $(B)/test_core
	$(B)/test_core

measure: $(B)/measure
	$(B)/measure $(if $(SAMPLES),--samples $(SAMPLES))

sweep: $(B)/measure
	$(B)/measure --sweep $(if $(SAMPLES),--samples $(SAMPLES))

coeffs: $(B)/gen_coeffs
	$(B)/gen_coeffs $(DESIGN) > src/smp_coeffs.c

check-coeffs: $(B)/gen_coeffs
	$(B)/gen_coeffs $(DESIGN) | cmp - src/smp_coeffs.c

target: | $(B)
	$(TCC) $(TFLAGS) -c -o $(B)/smp_core.o src/smp_core.c
	$(TCC) $(TFLAGS) -c -o $(B)/smp_coeffs.o src/smp_coeffs.c
	m68k-elf-ld -r -o $(B)/sampler.o $(B)/smp_core.o $(B)/smp_coeffs.o
	m68k-elf-size $(B)/smp_core.o $(B)/smp_coeffs.o
	@echo "undefined symbols (must be none):"
	@m68k-elf-nm -u $(B)/sampler.o

clean:
	rm -rf $(B)
