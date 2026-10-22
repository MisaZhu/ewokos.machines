/*
 * nvprobe - Q8B user-space NVMe bring-up and read-throughput probe.
 *
 *   nvprobe                 scan both M.2 slots, open the first controller
 *   nvprobe bench [MB]      read benchmark (default 64 MB)
 *   nvprobe verify [MB]     read twice with QD1/QD32 and compare
 *
 * Reads only: the NVMe may hold a live filesystem, so this tool never
 * issues a write unless asked to (there is no write path here).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/kernel_tic.h>
#include <bsp/nvme.h>

#define DEFAULT_BENCH_MB 64

static const char* slot_name(int slot) {
	return slot == NVME_SLOT_PCIE2A ? "pcie2a (M-key x4)"
		: "pcie3b (M-key x2)";
}

static void print_pci(const nvme_pci_dev_t* d) {
	printf("  %s  %02x:%02x.%x  %04x:%04x  class=%06x  "
			"bar0=%llx(bus)->%llx(cpu)%s  cmd=%04x\n",
			slot_name(d->slot), d->bus, d->dev, d->fn,
			d->vid, d->did, d->class_code,
			(unsigned long long)d->bar0_bus,
			(unsigned long long)d->bar0_phys,
			d->bar0_64bit ? " 64bit" : "",
			d->command);
	{
		uint32_t sp = 0, wd = 0;
		if (q8b_pcie_link(d->slot, 0, 0, 0, &sp, &wd) == 0)
			printf("    root port link: speed=%u width=x%u\n", sp, wd);
		if (q8b_pcie_link(d->slot, d->bus, d->dev, d->fn, &sp, &wd) == 0)
			printf("    endpoint link : speed=%u width=x%u\n", sp, wd);
	}
}

static int scan_slot(int slot, nvme_pci_dev_t* first) {
	nvme_pci_dev_t devs[8];
	int n = q8b_pcie_scan(slot, devs, 8);

	printf("%s: %d device(s)\n", slot_name(slot), n);
	for (int i = 0; i < n; i++)
		print_pci(&devs[i]);

	if (n > 0 && first->bar0_phys == 0)
		*first = devs[0];
	return n;
}

static uint64_t ns_now(void) {
	uint64_t ns = 0;
	if (kernel_tic_nsec(&ns) != 0)
		return 0;
	return ns;
}

static double mbps(uint64_t bytes, uint64_t ns) {
	if (ns == 0)
		return 0;
	return (double)bytes / ((double)ns / 1e9) / (1024.0 * 1024.0);
}

static int run_bench(uint64_t bytes, uint32_t chunk, uint32_t depth,
		bool copy_out, void* dst) {
	uint64_t t0 = ns_now();
	int r = nvme_read_stream(0, dst, bytes, chunk, depth, copy_out);
	uint64_t t1 = ns_now();

	if (r < 0) {
		printf("  chunk=%6u qd=%2u copy=%d  FAILED (%d)\n", chunk, depth,
				copy_out, r);
		return -1;
	}
	uint64_t done = (uint64_t)r;
	uint64_t ns = t1 - t0;
	printf("  chunk=%6u qd=%2u copy=%d  %6.1f MiB/s  (%llu bytes, %llu us)\n",
			chunk, depth, copy_out, mbps(done, ns),
			(unsigned long long)done, (unsigned long long)(ns / 1000));
	return 0;
}


/* Read the NVMe's GPT through the driver and print the partitions.  This
 * exercises the normal block path on real on-disk data (read-only). */
static void print_guid(const uint8_t* g) {
	printf("%08x-%04x-%04x-%02x%02x-", g[3] | (g[2] << 8) | (g[1] << 16) |
			((uint32_t)g[0] << 24),
			g[5] | (g[4] << 8), g[7] | (g[6] << 8), g[8], g[9]);
	for (int i = 10; i < 16; i++)
		printf("%02x", g[i]);
}

static void gpt_name(const uint8_t* name, char* out, int cap) {
	int n = 0;
	for (int i = 0; i + 1 < cap && i < 36; i++) {
		uint16_t c = (uint16_t)(name[i * 2] | (name[i * 2 + 1] << 8));
		if (c == 0)
			break;
		out[n++] = (c < 0x80) ? (char)c : '?';
	}
	out[n] = 0;
}

static int show_gpt(nvme_info_t* info) {
	uint8_t hdr[512];
	uint8_t* entries;
	if (nvme_read(1, hdr, 1) < 0) {
		printf("nvprobe: GPT header read failed\n");
		return -1;
	}
	uint64_t sig = 0;
	for (int i = 7; i >= 0; i--)
		sig = (sig << 8) | hdr[i];
	if (sig != 0x5452415020494645ull) {
		printf("nvprobe: no GPT signature at LBA1 (first 32 bytes):\n");
		for (int i = 0; i < 32; i++)
			printf("%02x%s", hdr[i], (i % 16 == 15) ? "\n" : " ");
		/* No GPT: parse the MBR partition table and verify that a
		 * partition boot sector can be read back through the driver. */
		uint8_t mbr[512];
		if (nvme_read(0, mbr, 1) != 1) {
			printf("nvprobe: LBA0 read failed\n");
			return -1;
		}
		printf("nvprobe: LBA0 first 16 bytes:");
		for (int i = 0; i < 16; i++)
			printf(" %02x", mbr[i]);
		printf("\n");
		printf("nvprobe: MBR signature 0x%02x%02x\n",
				mbr[511], mbr[510]);
		if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
			printf("nvprobe: no MBR and no GPT (blank drive?)\n");
			return -1;
		}
		for (int i = 0; i < 4; i++) {
			const uint8_t* e = mbr + 0x1be + i * 16;
			uint8_t type = e[4];
			if (type == 0)
				continue;
			uint64_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
					((uint32_t)e[10] << 16) |
					((uint32_t)e[11] << 24);
			uint64_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
					((uint32_t)e[14] << 16) |
					((uint32_t)e[15] << 24);
			printf("nvprobe: MBR part%d type=0x%02x lba=%llu sectors=%llu\n",
					i + 1, type, (unsigned long long)lba,
					(unsigned long long)cnt);
			/* Peek at the boot sector: NTFS/exFAT/FAT labels. */
			if (type == 0x07 || type == 0x0b || type == 0x0c ||
					type == 0x83) {
				uint8_t bs[512];
				if (nvme_read(lba, bs, 1) == 1) {
					char fs[9];
					memcpy(fs, bs + 3, 8);
					fs[8] = 0;
					printf("        fs=%.8s", fs);
					if (type == 0x07) {
						char label[12];
						memcpy(label, bs + 0x47, 11);
						label[11] = 0;
						printf(" label='%s'", label);
					}
					printf("\n");
				}
			}
		}
		return 0;
	}
	uint64_t entry_lba = 0;
	for (int i = 7; i >= 0; i--)
		entry_lba = (entry_lba << 8) | hdr[72 + i];
	uint32_t count = (uint32_t)(hdr[80] | (hdr[81] << 8) |
			(hdr[82] << 16) | (hdr[83] << 24));
	uint32_t esize = (uint32_t)(hdr[84] | (hdr[85] << 8) |
			(hdr[86] << 16) | (hdr[87] << 24));
	if (count == 0 || esize < 128 || count > 256 || entry_lba > info->nsze) {
		printf("nvprobe: unsupported GPT (count=%u esize=%u)\n", count, esize);
		return -1;
	}
	uint32_t sectors = (count * esize + 511) / 512;
	entries = (uint8_t*)malloc((size_t)sectors * 512);
	if (entries == NULL)
		return -1;
	if (nvme_read(entry_lba, entries, sectors) < 0) {
		printf("nvprobe: GPT entries read failed\n");
		free(entries);
		return -1;
	}
	printf("nvprobe: GPT entries=%u entry-size=%u\n", count, esize);
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t* e = entries + (size_t)i * esize;
		int used = 0;
		for (int j = 0; j < 16; j++)
			used |= e[j];
		if (!used)
			continue;
		uint64_t first = 0, last = 0;
		for (int j = 7; j >= 0; j--) {
			first = (first << 8) | e[32 + j];
			last = (last << 8) | e[40 + j];
		}
		char nm[40];
		gpt_name(e + 56, nm, sizeof(nm));
		printf("  part%u type=", i + 1);
		print_guid(e);
		printf(" first=%llu last=%llu '%s'\n",
				(unsigned long long)first,
				(unsigned long long)last, nm);
	}
	free(entries);
	return 0;
}

int main(int argc, char** argv) {
	nvme_pci_dev_t dev;
	nvme_info_t info;
	uint64_t bench_mb = DEFAULT_BENCH_MB;
	const char* mode = argc > 1 ? argv[1] : "scan";

	setbuf(stdout, NULL);
	memset(&dev, 0, sizeof(dev));

	printf("nvprobe: scanning Q8B M.2 slots\n");
	int n2a = scan_slot(NVME_SLOT_PCIE2A, &dev);
	/* pcie3b is only touched on demand: if the slot is empty the
	 * firmware powers the root complex down and config-space reads
	 * raise an external abort (SError). */
	int n3b = 0;
	if (n2a <= 0 || (argc > 1 && strcmp(argv[1], "scan3b") == 0))
		n3b = scan_slot(NVME_SLOT_PCIE3B, &dev);
	if (n2a <= 0 && n3b <= 0) {
		printf("nvprobe: no NVMe controller found\n");
		return 1;
	}
	if (argc > 2)
		bench_mb = strtoull(argv[2], NULL, 0);

	printf("\nnvprobe: opening %s %02x:%02x.%x\n",
			slot_name(dev.slot), dev.bus, dev.dev, dev.fn);
	if (nvme_open(&info, &dev) != 0) {
		printf("nvprobe: nvme_open failed\n");
		return 1;
	}
	printf("  model   : %s\n", info.model);
	printf("  serial  : %s\n", info.serial);
	printf("  firmware: %s\n", info.fw_rev);
	printf("  nsze    : %llu blocks x %u B = %llu MiB\n",
			(unsigned long long)info.nsze, info.block_size,
			(unsigned long long)((info.nsze * info.block_size) >>
				(20)));
	printf("  mdts    : %u (max single transfer %u KiB)\n", info.mdts,
			info.max_xfer / 1024);

	if (strcmp(mode, "scan") == 0) {
		nvme_close();
		return 0;
	}

	if (strcmp(mode, "gpt") == 0) {
		int r = show_gpt(&info);
		nvme_close();
		return r == 0 ? 0 : 1;
	}

	/* 16 MiB cached destination for copy-out / verify runs. */
	uint64_t verify_bytes = bench_mb * 1024ull * 1024ull;
	if (verify_bytes < 16ull * 1024 * 1024)
		verify_bytes = 16ull * 1024 * 1024;
	uint8_t* a = (uint8_t*)malloc(verify_bytes);
	uint8_t* b = (uint8_t*)malloc(verify_bytes);
	if (a == NULL || b == NULL) {
		printf("nvprobe: out of memory\n");
		nvme_close();
		return 1;
	}

	if (strcmp(mode, "verify") == 0) {
		int r1 = nvme_read_stream(0, a, verify_bytes, 128 * 1024, 1, true);
		int r2 = nvme_read_stream(0, b, verify_bytes, 128 * 1024, 32, true);
		printf("nvprobe: verify qd1=%d qd32=%d ", r1, r2);
		if (r1 == r2 && r1 > 0 && memcmp(a, b, (size_t)r1) == 0)
			printf("IDENTICAL\n");
		else
			printf("MISMATCH\n");
		free(a);
		free(b);
		nvme_close();
		return (r1 == r2 && r1 > 0) ? 0 : 1;
	}

	if (strcmp(mode, "bench") != 0) {
		printf("usage: nvprobe [scan|gpt|bench [MB]|verify [MB]]\n");
		free(a);
		free(b);
		nvme_close();
		return 1;
	}

	/* Make sure the bench range fits the namespace. */
	if (bench_mb * 1024ull * 1024ull > info.nsze * info.block_size)
		bench_mb = (info.nsze * info.block_size) >> 20;
	uint64_t bench_bytes = bench_mb * 1024ull * 1024ull;
	bench_bytes &= ~((uint64_t)info.block_size - 1);
	printf("\nnvprobe: benchmark %.0f MiB from LBA 0\n", (double)bench_mb);

	printf("-- raw (destination stays in the DMA arena, no copy) --\n");
	uint32_t chunks[] = { 64 * 1024, 128 * 1024, 256 * 1024 };
	uint32_t depths[] = { 1, 4, 8, 16, 31 };
	for (unsigned ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
		if (chunks[ci] > info.max_xfer)
			continue;
		for (unsigned di = 0; di < sizeof(depths) / sizeof(depths[0]);
				di++) {
			run_bench(bench_bytes, chunks[ci], depths[di], false,
					NULL);
		}
	}

	printf("-- copy-out (into cached malloc buffer, practical path) --\n");
	for (unsigned di = 0; di < sizeof(depths) / sizeof(depths[0]); di++) {
		run_bench(bench_bytes, info.max_xfer, depths[di], true, a);
	}

	/* Integrity: same range read with two queue depths must match. */
	int r1 = nvme_read_stream(0, a, verify_bytes, info.max_xfer, 1, true);
	int r2 = nvme_read_stream(0, b, verify_bytes, info.max_xfer, 32, true);
	printf("-- integrity qd1=%d qd32=%d: %s\n", r1, r2,
			(r1 == r2 && r1 > 0 && memcmp(a, b, (size_t)r1) == 0) ?
			"IDENTICAL" : "MISMATCH");

	free(a);
	free(b);
	nvme_close();
	return 0;
}
