// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <fcntl.h>
#include "nvme.h"
#include "plugin.h"
#include "nvme-print.h"

#define CREATE_CMD
#include "feat-nvme.h"

struct perfc_config {
	__u32 namespace_id;
	__u8 attri;
	bool rvspa;
	__u8 r4karl;
	char *paid;
	__u16 attrl;
	char *vs_data;
	bool save;
	__u8 sel;
};

static const char *power_mgmt_feat = "power management feature";
static const char *sel = "[0-3]: current/default/saved/supported";
static const char *save = "Specifies that the controller shall save the attribute";
static const char *perfc_feat = "performance characteristics feature";

static int power_mgmt_get(struct nvme_dev *dev, const __u8 fid, __u8 sel)
{
	__u32 result;
	int err;

	struct nvme_get_features_args args = {
		.args_size	= sizeof(args),
		.fd		= dev_fd(dev),
		.fid		= fid,
		.sel		= sel,
		.timeout	= NVME_DEFAULT_IOCTL_TIMEOUT,
		.result		= &result,
	};

	err = nvme_get_features(&args);
	if (!err) {
		if (NVME_CHECK(sel, GET_FEATURES_SEL, SUPPORTED))
			nvme_show_select_result(fid, result);
		else
			nvme_feature_show_fields(fid, result, NULL);
	} else {
		nvme_show_error("Get %s", power_mgmt_feat);
	}

	return err;
}

static int power_mgmt_set(struct nvme_dev *dev, const __u8 fid, __u8 ps, __u8 wh, bool save)
{
	__u32 result;
	int err;

	struct nvme_set_features_args args = {
		.args_size = sizeof(args),
		.fd = dev_fd(dev),
		.fid = fid,
		.cdw11 = NVME_SET(ps, FEAT_PWRMGMT_PS) | NVME_SET(wh, FEAT_PWRMGMT_WH),
		.save = save,
		.timeout = NVME_DEFAULT_IOCTL_TIMEOUT,
		.result = &result,
	};

	err = nvme_set_features(&args);

	nvme_show_init();

	if (err > 0) {
		nvme_show_status(err);
	} else if (err < 0) {
		nvme_show_perror("Set %s", power_mgmt_feat);
	} else {
		nvme_show_result("Set %s: 0x%04x (%s)", power_mgmt_feat, args.cdw11,
				 save ? "Save" : "Not save");
		nvme_feature_show_fields(fid, args.cdw11, NULL);
	}

	nvme_show_finish();

	return err;
}

static int feat_power_mgmt(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *ps = "power state";
	const char *wh = "workload hint";
	const __u8 fid = NVME_FEAT_FID_POWER_MGMT;

	_cleanup_nvme_dev_ struct nvme_dev *dev = NULL;
	int err;

	struct config {
		__u8 ps;
		__u8 wh;
		bool save;
		__u8 sel;
	};

	struct config cfg = { 0 };

	NVME_ARGS(opts,
		  OPT_BYTE("ps", 'p', &cfg.ps, ps),
		  OPT_BYTE("wh", 'w', &cfg.wh, wh),
		  OPT_FLAG("save", 's', &cfg.save, save),
		  OPT_BYTE("sel", 'S', &cfg.sel, sel));

	err = parse_and_open(&dev, argc, argv, POWER_MGMT_DESC, opts);
	if (err)
		return err;

	if (argconfig_parse_seen(opts, "ps"))
		err = power_mgmt_set(dev, fid, cfg.ps, cfg.wh, cfg.save);
	else
		err = power_mgmt_get(dev, fid, cfg.sel);

	return err;
}

static int perfc_get(struct nvme_dev *dev, struct perfc_config *cfg)
{
	__u32 result;
	int err;

	struct nvme_get_features_args args = {
		.args_size = sizeof(args),
		.fd = dev_fd(dev),
		.fid = NVME_FEAT_FID_PERF_CHARACTERISTICS,
		.cdw11 = NVME_SET(cfg->attri, FEAT_PERFC_ATTRI) |
			 NVME_SET(cfg->rvspa, FEAT_PERFC_RVSPA),
		.sel = cfg->sel,
		.timeout = NVME_DEFAULT_IOCTL_TIMEOUT,
		.result	 = &result,
	};

	err = nvme_get_features(&args);
	if (!err) {
		if (NVME_CHECK(args.sel, GET_FEATURES_SEL, SUPPORTED))
			nvme_show_select_result(args.fid, result);
		else
			nvme_feature_show_fields(args.fid, result, NULL);
	} else {
		nvme_show_error("Get %s", perfc_feat);
	}

	return err;
}

static int perfc_set(struct nvme_dev *dev, struct perfc_config *cfg)
{
	__u32 result;
	int err;

	_cleanup_fd_ int ffd = STDIN_FILENO;

	struct nvme_perf_characteristics data = {
		.attr_buf = { 0 },
	};

	struct nvme_set_features_args args = {
		.args_size = sizeof(args),
		.fd = dev_fd(dev),
		.fid = NVME_FEAT_FID_PERF_CHARACTERISTICS,
		.cdw11 = NVME_SET(cfg->attri, FEAT_PERFC_ATTRI) |
			 NVME_SET(cfg->rvspa, FEAT_PERFC_RVSPA),
		.save = save,
		.timeout = NVME_DEFAULT_IOCTL_TIMEOUT,
		.result = &result,
		.data = &data,
		.data_len = sizeof(data),
	};

	switch (cfg->attri) {
	case NVME_FEAT_PERFC_ATTRI_STD:
		data.std_perf->r4karl = cfg->r4karl;
		break;
	case NVME_FEAT_PERFC_ATTRI_VS_MIN ... NVME_FEAT_PERFC_ATTRI_VS_MAX:
		nvme_uuid_from_string(cfg->paid, data.vs_perf->paid);
		data.vs_perf->attrl = cfg->attrl;
		if (data.vs_perf->attrl && strlen(cfg->vs_data)) {
			ffd = open(cfg->vs_data, O_RDONLY);
			if (ffd < 0) {
				nvme_show_error("Failed to open file %s: %s", cfg->vs_data,
						strerror(errno));
				return -EINVAL;
			}
			err = read(ffd, data.vs_perf->vs, data.vs_perf->attrl);
			if (err < 0) {
				nvme_show_error("failed to read data buffer from input file: %s",
						strerror(errno));
				return -errno;
			}
		}
		break;
	default:
		break;
	}

	err = nvme_set_features(&args);

	nvme_show_init();

	if (err > 0) {
		nvme_show_status(err);
	} else if (err < 0) {
		nvme_show_perror("Set %s", perfc_feat);
	} else {
		nvme_show_result("Set %s: 0x%04x (%s)", perfc_feat, args.cdw11,
				 save ? "Save" : "Not save");
		nvme_feature_show_fields(args.fid, args.cdw11, NULL);
	}

	nvme_show_finish();

	return err;
}

static int feat_perfc(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *namespace_id_optional = "optional namespace attached to controller";
	const char *attri = "attribute index";
	const char *rvspa = "revert vendor specific performance attribute";
	const char *r4karl = "random 4 kib average read latency";
	const char *paid = "performance attribute identifier";
	const char *attrl = "attribute length";
	const char *vs_data = "vendor specific data";

	_cleanup_nvme_dev_ struct nvme_dev *dev = NULL;
	int err;

	struct perfc_config cfg = { 0 };

	NVME_ARGS(opts,
		  OPT_UINT("namespace-id", 'n', &cfg.namespace_id, namespace_id_optional),
		  OPT_BYTE("attri", 'a', &cfg.attri, attri),
		  OPT_FLAG("rvspa", 'r', &cfg.rvspa, rvspa),
		  OPT_BYTE("r4karl", 'R', &cfg.r4karl, r4karl),
		  OPT_STR("paid", 'p', &cfg.paid, paid),
		  OPT_SHRT("attrl", 'A', &cfg.attrl, attrl),
		  OPT_FILE("vs-data", 'V', &cfg.vs_data, vs_data),
		  OPT_FLAG("save", 's', &cfg.save, save),
		  OPT_BYTE("sel", 'S', &cfg.sel, sel));

	err = parse_and_open(&dev, argc, argv, PERFC_DESC, opts);
	if (err)
		return err;

	if (argconfig_parse_seen(opts, "rvspa") || argconfig_parse_seen(opts, "r4karl") ||
	    argconfig_parse_seen(opts, "paid"))
		err = perfc_set(dev, &cfg);
	else
		err = perfc_get(dev, &cfg);

	return err;
}
