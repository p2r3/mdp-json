#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"
#include "config.h"
#include "demo.h"

#define EXPECTED_MAPS_FILE "expected_maps.txt"
#define CMD_WHITELIST_FILE "cmd_whitelist.txt"
#define SAR_WHITELIST_FILE "sar_whitelist.txt"
#define CVAR_WHITELIST_FILE "cvar_whitelist.txt"
#define FILESUM_WHITELIST_FILE "filesum_whitelist.txt"
#define GENERAL_CONF_FILE "config.txt"

static char **g_cmd_whitelist;
static char **g_sar_sum_whitelist;
static struct var_whitelist *g_filesum_whitelist;
static struct var_whitelist *g_cvar_whitelist;

// general config options
struct {
	int file_sum_mode; // 0 = don't show, 1 = show not matching, 2 (default) = show not matching or not present
	int initial_cvar_mode; // 0 = don't show, 1 = show not matching, 2 (default) = show not matching or not present
	bool show_passing_checksums; // should we output successful checksums?
	bool show_wait; // should we show when 'wait' was run?
	bool show_splits; // should we show split times?
	int show_netmessages; // 0 = don't show, 1 = show all except srtimer, 2 = show all
} g_config;

static bool _allow_initial_cvar(const char *var, const char *val) {
#define ALLOW(x, y) if (!strcmp(var, #x) && !strcmp(val, y)) return true
#define ALLOWINT(x, y) if (!strcmp(var, #x) && atoi(val) == y) return true
#define ALLOWRANGE(x, y, z) if (!strcmp(var, #x) && atoi(val) >= y && atoi(val) <= z) return true

	ALLOWINT(host_timescale, 1);
	ALLOWINT(sv_alternateticks, 1);
	ALLOWINT(sv_allow_mobile_portals, 0);
	ALLOWINT(sv_portal_placement_debug, 0);
	ALLOWINT(cl_cmdrate, 30);
	ALLOWINT(cl_updaterate, 20);
	ALLOWRANGE(cl_fov, 45, 140);
	ALLOWRANGE(fps_max, 30, 999);
	ALLOW(sv_use_trace_duration, "0.5");
	ALLOW(m_yaw, "0.022");

	return false;

#undef ALLOW
#undef ALLOWINT
#undef ALLOWRANGE
}

static bool _ignore_filesum(const char *path) {
	// hack to deal with the fact that older sar versions included way
	// more filesums than now
	size_t len = strlen(path);
	const char *end = path + len;
	if (len > 3 && !strcasecmp(end - 3, ".so")) return true;
	if (len > 4 && !strcasecmp(end - 4, ".dll")) return true;
	if (len > 4 && !strcasecmp(end - 4, ".bsp")) return true;
	if (len > 4 && !strcasecmp(end - 4, ".vpk")) {
		if (!strncmp(path, "./portal2_dlc1/", 15)) return true;
		if (!strncmp(path, "./portal2_dlc2/", 15)) return true;
		if (!strncmp(path, "portal2_dlc1/", 13)) return true;
		if (!strncmp(path, "portal2_dlc2/", 13)) return true;
	}
	return false;
}

// janky hack lol
static const char *const _g_map_found = "_MAP_FOUND";
static const char **_g_expected_maps;

static void _output_sar_data(struct demo *demo, uint32_t tick, struct sar_data data) {
	switch (data.type) {
	case SAR_DATA_TIMESCALE_CHEAT:
		printf("\t\t\t\t{ \"tick\": %d, \"type\": \"timescale\", \"value\": \"%.2f\" },\n", tick, data.timescale);
		break;
	case SAR_DATA_INITIAL_CVAR:
		if (g_config.initial_cvar_mode != 0) {
			int whitelist_status = config_check_var_whitelist(g_cvar_whitelist, data.initial_cvar.cvar, data.initial_cvar.val);
			if (!_allow_initial_cvar(data.initial_cvar.cvar, data.initial_cvar.val) && (whitelist_status == 1 || (whitelist_status == 0 && g_config.initial_cvar_mode == 2))) {
				for (size_t i = 0; i < strlen(data.initial_cvar.val); ++i) {
					if (data.initial_cvar.val[i] == '"') data.initial_cvar.val[i] = '\'';
				}
				printf("\t\t\t\t{ \"tick\": %d, \"type\": \"cvar\", \"value\": { \"cvar\": \"%s\", \"value\": \"%s\" } },\n", tick, data.initial_cvar.cvar, data.initial_cvar.val);
			}
		}
		break;
	case SAR_DATA_PAUSE:
		printf("\t\t\t\t{ \"tick\": %d, \"type\": \"pause\", \"value\": { \"ticks\": %d, \"timed\": %s } },\n", tick, data.pause_time.ticks, data.pause_time.timed == -1 ? "null" : (data.pause_time.timed ? "true" : "false"));
		break;
	case SAR_DATA_INVALID:
		printf("\t\t\t\t{ \"tick\": %d, \"type\": \"invalid\" },\n", tick);
		break;
	case SAR_DATA_WAIT_RUN:
		if (g_config.show_wait) printf("\t\t\t\t{ \"tick\": %d, \"type\": \"wait\", \"value\": %d },\n", tick, data.wait_run.tick);
		break;
	case SAR_DATA_HWAIT_RUN:
		if (g_config.show_wait) printf("\t\t\t\t{ \"tick\": %d, \"type\": \"hwait\", \"value\": %d },\n", tick, data.hwait_run.ticks);
		break;
	case SAR_DATA_ENTITY_SERIAL:
		printf("\t\t\t\t{ \"tick\": %d, \"type\": \"serial\", \"value\": { \"slot\": %d, \"serial\": %d } },\n", tick, data.entity_serial.slot, data.entity_serial.serial);
		break;
	case SAR_DATA_FRAMETIME:
		printf("\t\t\t\t{ \"tick\": %d, \"type\": \"frametime\", \"value\": %f },\n", tick, data.frametime * 1000.0f);
		break;
	case SAR_DATA_SPEEDRUN_TIME:
		if (g_config.show_splits) {
			printf("\t\t\t\t{ \"tick\": %d, \"type\": \"speedrun\", \"value\":\n\t\t\t\t\t{\n\t\t\t\t\t\t\"splits\": [\n", tick);
			size_t ticks = 0;
			for (size_t i = 0; i < data.speedrun_time.nsplits; ++i) {
				printf("\t\t\t\t\t\t\t{ \"name\": \"%s\", \"segments\":\n\t\t\t\t\t\t\t\t[\n", data.speedrun_time.splits[i].name);
				for (size_t j = 0; j < data.speedrun_time.splits[i].nsegs; ++j) {
					printf("\t\t\t\t\t\t\t\t\t{ \"name\": \"%s\", \"ticks\": %d }", data.speedrun_time.splits[i].segs[j].name, data.speedrun_time.splits[i].segs[j].ticks);
					if (j != data.speedrun_time.splits[i].nsegs - 1) printf(",");
					printf("\n");
					ticks += data.speedrun_time.splits[i].segs[j].ticks;
				}
				printf("\t\t\t\t\t\t\t\t]\n\t\t\t\t\t\t\t}");
				if (i != data.speedrun_time.nsplits - 1) printf(",");
				printf("\n");
			}
			printf("\t\t\t\t\t\t],\n\t\t\t\t\t\t\"rules\": [\n");
			for (size_t i = 0; i < data.speedrun_time.nrules; ++i) {
				for (size_t j = 0; j < strlen(data.speedrun_time.rules[i].data); ++j) {
					if (data.speedrun_time.rules[i].data[j] == '"') data.speedrun_time.rules[i].data[j] = '\'';
				}
				printf("\t\t\t\t\t\t\t{ \"name\": \"%s\", \"data\": \"%s\" }", data.speedrun_time.rules[i].name, data.speedrun_time.rules[i].data);
				if (i != data.speedrun_time.nrules - 1) printf(",");
				printf("\n");
			}
			printf("\t\t\t\t\t\t],\n");

			size_t total = roundf((float)(ticks * 1000) / demo->tickrate);

			int ms = total % 1000;
			total /= 1000;
			int secs = total % 60;
			total /= 60;
			int mins = total % 60;
			total /= 60;
			int hrs = total;

			printf("\t\t\t\t\t\t\"total\": { \"ticks\": %zu, \"time\": \"%d:%02d:%02d.%03d\" }\n\t\t\t\t\t}\n\t\t\t\t},\n", ticks, hrs, mins, secs, ms);
		}
		break;
	case SAR_DATA_TIMESTAMP:
		printf(
			"\t\t\t\t{ \"tick\": %d, \"type\": \"timestamp\", \"value\": \"%04d/%02d/%02d %02d:%02d:%02d UTC\" },\n",
			tick,
			(int)data.timestamp.year,
			(int)data.timestamp.mon,
			(int)data.timestamp.day,
			(int)data.timestamp.hour,
			(int)data.timestamp.min,
			(int)data.timestamp.sec
		);
		break;
	case SAR_DATA_FILE_CHECKSUM:
		if (g_config.file_sum_mode != 0) {
			char strbuf[9];
			snprintf(strbuf, sizeof strbuf, "%08X", data.file_checksum.sum);
			int whitelist_status = config_check_var_whitelist(g_filesum_whitelist, data.file_checksum.path, strbuf);
			if (!_ignore_filesum(data.file_checksum.path) && (whitelist_status == 1 || (whitelist_status == 0 && g_config.file_sum_mode == 2))) {
				printf("\t\t\t\t{ \"tick\": %d, \"type\": \"file\", \"value\": { \"path\": \"%s\", \"sum\": \"%08X\" } },\n", tick, data.file_checksum.path, data.file_checksum.sum);
			}
		}
		break;
	case SAR_DATA_PORTAL_PLACEMENT:
		printf("\t\t\t\t{ \"tick\": %d, \"type\": \"portal\", \"value\": %d },\n", tick, data.portal_placement.orange);
		break;
	case SAR_DATA_QUEUEDCMD:
		if (!config_check_cmd_whitelist(g_cmd_whitelist, data.queuedcmd)) {
			for (size_t i = 0; i < strlen(data.queuedcmd); ++i) {
				if (data.queuedcmd[i] == '"') data.queuedcmd[i] = '\'';
			}
			printf("\t\t\t\t{ \"tick\": %d, \"type\": \"queuedcmd\", \"value\": \"%s\" },\n", tick, data.queuedcmd);
		}
		break;
	default:
		// don't care
		break;
	}
}

#define SAR_MSG_INIT_B "&^!$"
#define SAR_MSG_INIT_O "&^!%"
#define SAR_MSG_CONT_B "&^?$"
#define SAR_MSG_CONT_O "&^?%"

static char g_partial[8192];
static int g_expected_len = 0;

///// START BASE92 /////

// This isn't really base92. Instead, we encode 4-byte input chunks into 5 base92 characters. If the
// final chunk is not 4 bytes, each byte of it is sent as 2 base92 characters; the receiver infers
// this from the buffer length and decodes accordingly. This system is almost as space-efficient as
// is possible.

static char base92_chars[93] = // 93 because null terminator
	"abcdefghijklmnopqrstuvwxyz"
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"0123456789"
	"!$%^&*-_=+()[]{}<>'@#~;:/?,.|\\";

// doing this at runtime is a little silly but shh
static const char *base92_reverse() {
	static char map[256];
	static int initd = 0;
	if (initd == 0) {
		initd = 1;
		for (int i = 0; i < 92; ++i) {
			char c = base92_chars[i];
			map[(int)c] = i;
		}
	}
	return map;
}

static char *base92_decode(const char *encoded, int len) {
	const char *base92_rev = base92_reverse();

	static char out[512]; // leave some overhead lol
	static int outLen;
	memset(out, 0, sizeof(out));
	outLen = 0;
	#define push(val) \
		out[outLen++] = val;
	while (len > 6 || len == 5) {
		unsigned val = base92_rev[(unsigned)encoded[4]];
		val = (val * 92) + base92_rev[(unsigned)encoded[3]];
		val = (val * 92) + base92_rev[(unsigned)encoded[2]];
		val = (val * 92) + base92_rev[(unsigned)encoded[1]];
		val = (val * 92) + base92_rev[(unsigned)encoded[0]];

		char *raw = (char *)&val;
		push(raw[0]);
		push(raw[1]);
		push(raw[2]);
		push(raw[3]);

		encoded += 5;
		len -= 5;
	}
	while (len > 0) {
		unsigned val = base92_rev[(unsigned)encoded[1]];
		val = (val * 92) + base92_rev[(unsigned)encoded[0]];

		char *raw = (char *)&val;
		push(raw[0]);
		encoded += 2;
		len -= 2;
	}
	return out;
}

///// END BASE92 /////

static bool handleMessage(struct demo_msg *msg) {
	if (strncmp(msg->con_cmd, "say \"", 5)) return false;
	if (strlen(msg->con_cmd) < 10) return false;
	bool has_prefix = true;
	char *prefix = msg->con_cmd + 5;
	bool cont, orange;
	if (!strncmp(prefix, SAR_MSG_INIT_B, 4)) {
		cont = false;
		orange = false;
	} else if (!strncmp(prefix, SAR_MSG_INIT_O, 4)) {
		cont = false;
		orange = true;
	} else if (!strncmp(prefix, SAR_MSG_CONT_B, 4)) {
		cont = true;
		orange = false;
	} else if (!strncmp(prefix, SAR_MSG_CONT_O, 4)) {
		cont = true;
		orange = true;
	} else {
		has_prefix = false;
	}

	if (!has_prefix) return false;

	if (cont) {
		if (!g_expected_len) {
			// fprintf(g_errfile, "\t\t[%5u] Unmatched NetMessage continuation %s\n", msg->tick, msg->con_cmd);
			return false;
		}
		strcat(g_partial, msg->con_cmd + 9);
	} else {
		char *raw = base92_decode(msg->con_cmd + 9, 5);
		g_expected_len = (int)*raw;
		strcat(g_partial, msg->con_cmd + 14);
	}

	if (strlen(g_partial) < g_expected_len) {
		// fprintf(g_outfile, "\t\t[%5u] NetMessage continuation %d != %d\n", msg->tick, g_expected_len, (int)strlen(g_partial));
		return true;
	} else if (strlen(g_partial) > g_expected_len) {
		// fprintf(g_errfile, "\t\t[%5u] NetMessage length mismatch %d != %d\n", msg->tick, g_expected_len, (int)strlen(g_partial));
		return false;
	} else {
		char *decoded = base92_decode(g_partial, g_expected_len);
		char *type = decoded;
		char *data = decoded + strlen(type) + 1;

		if (g_config.show_netmessages == 2 || (g_config.show_netmessages == 1 && strcmp(type, "srtimer"))) {
			printf("\t\t\t\t{ \"tick\": %d, \"type\": \"netmessage\", \"value\": { \"player\": \"%s\", \"type\": \"%s\"", msg->tick, orange ? "o" : "b", type);

			// print data
			int datalen = strlen(data);
			bool printdata = true;
			if (!strcmp(type, "srtimer")) {
				datalen = 4;
				printdata = false;
			}
			if (!strcmp(type, "cmboard")) {
				datalen = 0;
			}

			if (datalen > 0) {
				if (printdata) printf(", \"data\": \"%s\", \"hex\": \"", data);
				else printf(", \"data\": null, \"hex\": \"");

				for (int i = 0; i < datalen; ++i) {
					printf("%02X", (unsigned char)data[i]);
				}
				printf("\"");
			} else {
				printf(", \"data\": null, \"hex\": null");
			}

			printf(" } },\n");
		}
	}
	g_expected_len = 0;
	g_partial[0] = 0;
	return true;
}

static void _output_msg(struct demo *demo, struct demo_msg *msg) {
	switch (msg->type) {
	case DEMO_MSG_CONSOLE_CMD:
		if (!config_check_cmd_whitelist(g_cmd_whitelist, msg->con_cmd)) {
			if (!handleMessage(msg)) {
				for (size_t i = 0; i < strlen(msg->con_cmd); ++i) {
					if (msg->con_cmd[i] == '"') msg->con_cmd[i] = '\'';
				}
				printf("\t\t\t\t{ \"tick\": %d, \"type\": \"cmd\", \"value\": \"%s\" },\n", msg->tick, msg->con_cmd);
			}
		}
		break;
	case DEMO_MSG_SAR_DATA:
		_output_sar_data(demo, msg->tick, msg->sar_data);
		break;
	default:
		break;
	}
}

static void _validate_checksum(uint32_t demo_given, uint32_t sar_given, uint32_t demo_real) {
	bool demo_matches = demo_given == demo_real;
	if (!demo_matches) {
		printf("\t\t\t\t{ \"tick\": 0, \"type\": \"demosum\", \"value\": { \"given\": \"%X\", \"real\": \"%X\" } },\n", demo_given, demo_real);
	}

	if (!config_check_sum_whitelist(g_sar_sum_whitelist, sar_given)) {
		printf("\t\t\t\t{ \"tick\": 0, \"type\": \"sarsum\", \"value\": \"%X\" },\n", sar_given);
	}
}

void run_demo(const char *path) {
	struct demo *demo = demo_parse(path);

	if (!demo) {
		printf("\t\tnull");
		return;
	}

	bool has_csum = false;

	printf("\t\t{\n");
	printf("\t\t\t\"file\": \"%s\",\n", path);
	printf("\t\t\t\"user\": \"%s\",\n", demo->hdr.client_name);
	printf("\t\t\t\"map\": \"%s\",\n", demo->hdr.map_name);
	printf("\t\t\t\"tps\": %.2f,\n", demo->tickrate);
	printf("\t\t\t\"ticks\": %d,\n", demo->hdr.playback_ticks);
	printf("\t\t\t\"events\": [\n");
	for (size_t i = 0; i < demo->nmsgs; ++i) {
		struct demo_msg *msg = demo->msgs[i];
		if (i == demo->nmsgs - 1 && msg->type == DEMO_MSG_SAR_DATA && msg->sar_data.type == SAR_DATA_CHECKSUM) {
			// ending checksum data - validate it
			_validate_checksum(msg->sar_data.checksum.demo_sum, msg->sar_data.checksum.sar_sum, demo->checksum);
			has_csum = true;
		} else {
			// normal message
			_output_msg(demo, msg);
		}
	}

	if (demo->v2sum_state == V2SUM_INVALID) {

		printf("\t\t\t\t{ \"tick\": 0, \"type\": \"demosum\", \"value\": null },\n");

	} else if (demo->v2sum_state == V2SUM_VALID) {

		struct demo_msg *msg = demo->msgs[demo->nmsgs - 1];
		uint32_t sar_sum = msg->sar_data.checksum_v2.sar_sum;

		if (!config_check_sum_whitelist(g_sar_sum_whitelist, sar_sum)) {
			printf("\t\t\t\t{ \"tick\": 0, \"type\": \"sarsum\", \"value\": \"%X\" },\n", sar_sum);
		}

	}
	// this really just compensates for the extra comma in the last event
	printf("\t\t\t\t{ \"tick\": 0, \"type\": \"end\", \"value\": null }\n\t\t\t]\n\t\t}\n");

	if (_g_expected_maps) {
		for (size_t i = 0; _g_expected_maps[i]; ++i) {
			if (_g_expected_maps[i] == _g_map_found) continue; // This pointer equality check is intentional
			if (!strcmp(_g_expected_maps[i], demo->hdr.map_name)) {
				_g_expected_maps[i] = _g_map_found;
			}
		}
	}

	if (!has_csum && demo->v2sum_state == V2SUM_NONE) {
		fprintf(stderr, "no checksums found; vanilla demo?\n");
	}

	demo_free(demo);
}

int main(int argc, char **argv) {
	_g_expected_maps = (const char **)config_read_newline_sep(EXPECTED_MAPS_FILE);
	g_cmd_whitelist = config_read_newline_sep(CMD_WHITELIST_FILE);
	g_cvar_whitelist = config_read_var_whitelist(CVAR_WHITELIST_FILE);

	// read filesum/sarsum paths from args if specified
	if (argc >= 2) {
		for (int i = 1; i < argc; i ++) {
			if (strcmp(argv[i], "--sarsum-path") == 0) {
				g_sar_sum_whitelist = config_read_newline_sep(argv[i + 1]);
				i ++;
			} else if (strcmp(argv[i], "--filesum-path") == 0) {
				g_filesum_whitelist = config_read_var_whitelist(argv[i + 1]);
				i ++;
			}
		}
	}
	// if one of the paths wasn't found in args, read from default file path
	if (g_sar_sum_whitelist == NULL) g_sar_sum_whitelist = config_read_newline_sep(SAR_WHITELIST_FILE);
	if (g_filesum_whitelist == NULL) g_filesum_whitelist = config_read_var_whitelist(FILESUM_WHITELIST_FILE);

	g_config.file_sum_mode = 2;
	g_config.initial_cvar_mode = 2;
	g_config.show_passing_checksums = false;
	g_config.show_wait = true;
	g_config.show_splits = true;
	g_config.show_netmessages = 2;
	struct var_whitelist *general_conf = config_read_var_whitelist(GENERAL_CONF_FILE);
	if (general_conf) {
		for (struct var_whitelist *ptr = general_conf; ptr->var_name; ++ptr) {
			if (!strcmp(ptr->var_name, "file_sum_mode")) {
				int val = atoi(ptr->val);
				if (val < 0) val = 0;
				if (val > 2) val = 2;
				g_config.file_sum_mode = val;
				continue;
			}

			if (!strcmp(ptr->var_name, "initial_cvar_mode")) {
				int val = atoi(ptr->val);
				if (val < 0) val = 0;
				if (val > 2) val = 2;
				g_config.initial_cvar_mode = val;
				continue;
			}

			if (!strcmp(ptr->var_name, "show_passing_checksums")) {
				int val = atoi(ptr->val);
				g_config.show_passing_checksums = val != 0;
				continue;
			}

			if (!strcmp(ptr->var_name, "show_wait")) {
				int val = atoi(ptr->val);
				g_config.show_wait = val != 0;
				continue;
			}

			if (!strcmp(ptr->var_name, "show_splits")) {
				int val = atoi(ptr->val);
				g_config.show_splits = val != 0;
				continue;
			}

			if (!strcmp(ptr->var_name, "show_netmessages")) {
				int val = atoi(ptr->val);
				if (val < 0) val = 0;
				if (val > 2) val = 2;
				g_config.show_netmessages = val;
				continue;
			}

			fprintf(stderr, "bad config option '%s'\n", ptr->var_name);
		}
		config_free_var_whitelist(general_conf);
	}

	if (argc >= 2) {
		printf("\n{\n\t\"demos\": [\n");
		for (int i = 1; i < argc; i ++) {
			if (strncmp(argv[i], "--", 2) == 0) {
				i ++;
				continue;
			}

			char *path = argv[i];
			if (i > 1) printf(",\n");
			run_demo(path);
		}
		printf("\t]\n}\n");
	} else {
		fprintf(stderr, "no demo file provided on command line\n");
	}

	if (_g_expected_maps) {
		bool did_hdr = false;
		for (size_t i = 0; _g_expected_maps[i]; ++i) {
			if (_g_expected_maps[i] == _g_map_found) continue; // This pointer equality check is intentional
			if (!did_hdr) {
				did_hdr = true;
				fprintf(stderr, "missing maps:\n");
			}
			fprintf(stderr, "\t%s\n", _g_expected_maps[i]);
		}
	}

	config_free_newline_sep(g_cmd_whitelist);
	config_free_newline_sep(g_sar_sum_whitelist);
	config_free_var_whitelist(g_filesum_whitelist);
	config_free_var_whitelist(g_cvar_whitelist);

	return 0;
}
