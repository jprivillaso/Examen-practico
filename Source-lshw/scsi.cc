#include "mem.h"
#include "cdrom.h"
#include "disk.h"
#include "osutils.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <scsi/sg.h>
#include <scsi/scsi.h>

#include <string>
#include <map>

#define SG_X "/dev/sg%d"

#ifndef SCSI_IOCTL_GET_PCI
#define SCSI_IOCTL_GET_PCI 0x5387
#endif

#define OPEN_FLAG O_RDWR

#define SENSE_BUFF_LEN 32
#define INQ_REPLY_LEN 96
#define INQ_CMD_CODE 0x12
#define INQ_CMD_LEN 6
#define INQ_PAGE_SERIAL 0x80

#define SENSE_REPLY_LEN 96
#define SENSE_CMD_CODE 0x1a
#define SENSE_CMD_LEN 6
#define SENSE_PAGE_SERIAL 0x80

#define MX_ALLOC_LEN 255
#define EBUFF_SZ 256

/* Some of the following error/status codes are exchanged between the
   various layers of the SCSI sub-system in Linux and should never
   reach the user. They are placed here for completeness. What appears
   here is copied from drivers/scsi/scsi.h which is not visible in
   the user space. */

#ifndef SCSI_CHECK_CONDITION
/* Following are the "true" SCSI status codes. Linux has traditionally
   used a 1 bit right and masked version of these. So now CHECK_CONDITION
   and friends (in <scsi/scsi.h>) are deprecated. */
#define SCSI_CHECK_CONDITION 0x2
#define SCSI_CONDITION_MET 0x4
#define SCSI_BUSY 0x8
#define SCSI_IMMEDIATE 0x10
#define SCSI_IMMEDIATE_CONDITION_MET 0x14
#define SCSI_RESERVATION_CONFLICT 0x18
#define SCSI_COMMAND_TERMINATED 0x22
#define SCSI_TASK_SET_FULL 0x28
#define SCSI_ACA_ACTIVE 0x30
#define SCSI_TASK_ABORTED 0x40
#endif

/* The following are 'host_status' codes */
#ifndef DID_OK
#define DID_OK 0x00
#endif
#ifndef DID_NO_CONNECT
#define DID_NO_CONNECT 0x01	/* Unable to connect before timeout */
#define DID_BUS_BUSY 0x02	/* Bus remain busy until timeout */
#define DID_TIME_OUT 0x03	/* Timed out for some other reason */
#define DID_BAD_TARGET 0x04	/* Bad target (id?) */
#define DID_ABORT 0x05		/* Told to abort for some other reason */
#define DID_PARITY 0x06		/* Parity error (on SCSI bus) */
#define DID_ERROR 0x07		/* Internal error */
#define DID_RESET 0x08		/* Reset by somebody */
#define DID_BAD_INTR 0x09	/* Received an unexpected interrupt */
#define DID_PASSTHROUGH 0x0a	/* Force command past mid-level */
#define DID_SOFT_ERROR 0x0b	/* The low-level driver wants a retry */
#endif

/* These defines are to isolate applictaions from kernel define changes */
#define SG_ERR_DID_OK           DID_OK
#define SG_ERR_DID_NO_CONNECT   DID_NO_CONNECT
#define SG_ERR_DID_BUS_BUSY     DID_BUS_BUSY
#define SG_ERR_DID_TIME_OUT     DID_TIME_OUT
#define SG_ERR_DID_BAD_TARGET   DID_BAD_TARGET
#define SG_ERR_DID_ABORT        DID_ABORT
#define SG_ERR_DID_PARITY       DID_PARITY
#define SG_ERR_DID_ERROR        DID_ERROR
#define SG_ERR_DID_RESET        DID_RESET
#define SG_ERR_DID_BAD_INTR     DID_BAD_INTR
#define SG_ERR_DID_PASSTHROUGH  DID_PASSTHROUGH
#define SG_ERR_DID_SOFT_ERROR   DID_SOFT_ERROR

/* The following are 'driver_status' codes */
#ifndef DRIVER_OK
#define DRIVER_OK 0x00
#endif
#ifndef DRIVER_BUSY
#define DRIVER_BUSY 0x01
#define DRIVER_SOFT 0x02
#define DRIVER_MEDIA 0x03
#define DRIVER_ERROR 0x04
#define DRIVER_INVALID 0x05
#define DRIVER_TIMEOUT 0x06
#define DRIVER_HARD 0x07
#define DRIVER_SENSE 0x08	/* Sense_buffer has been set */

/* Following "suggests" are "or-ed" with one of previous 8 entries */
#define SUGGEST_RETRY 0x10
#define SUGGEST_ABORT 0x20
#define SUGGEST_REMAP 0x30
#define SUGGEST_DIE 0x40
#define SUGGEST_SENSE 0x80
#define SUGGEST_IS_OK 0xff
#endif
#ifndef DRIVER_MASK
#define DRIVER_MASK 0x0f
#endif
#ifndef SUGGEST_MASK
#define SUGGEST_MASK 0xf0
#endif

/* These defines are to isolate applictaions from kernel define changes */
#define SG_ERR_DRIVER_OK        DRIVER_OK
#define SG_ERR_DRIVER_BUSY      DRIVER_BUSY
#define SG_ERR_DRIVER_SOFT      DRIVER_SOFT
#define SG_ERR_DRIVER_MEDIA     DRIVER_MEDIA
#define SG_ERR_DRIVER_ERROR     DRIVER_ERROR
#define SG_ERR_DRIVER_INVALID   DRIVER_INVALID
#define SG_ERR_DRIVER_TIMEOUT   DRIVER_TIMEOUT
#define SG_ERR_DRIVER_HARD      DRIVER_HARD
#define SG_ERR_DRIVER_SENSE     DRIVER_SENSE
#define SG_ERR_SUGGEST_RETRY    SUGGEST_RETRY
#define SG_ERR_SUGGEST_ABORT    SUGGEST_ABORT
#define SG_ERR_SUGGEST_REMAP    SUGGEST_REMAP
#define SG_ERR_SUGGEST_DIE      SUGGEST_DIE
#define SG_ERR_SUGGEST_SENSE    SUGGEST_SENSE
#define SG_ERR_SUGGEST_IS_OK    SUGGEST_IS_OK
#define SG_ERR_DRIVER_MASK      DRIVER_MASK
#define SG_ERR_SUGGEST_MASK     SUGGEST_MASK

/* The following "category" function returns one of the following */
#define SG_ERR_CAT_CLEAN 0	/* No errors or other information */
#define SG_ERR_CAT_MEDIA_CHANGED 1	/* interpreted from sense buffer */
#define SG_ERR_CAT_RESET 2	/* interpreted from sense buffer */
#define SG_ERR_CAT_TIMEOUT 3
#define SG_ERR_CAT_RECOVERED 4	/* Successful command after recovered err */
#define SG_ERR_CAT_SENSE 98	/* Something else is in the sense buffer */
#define SG_ERR_CAT_OTHER 99	/* Some other error/warning has occurred */

typedef struct my_sg_scsi_id
{
  int host_no;			/* as in "scsi<n>" where 'n' is one of 0, 1, 2 etc */
  int channel;
  int scsi_id;			/* scsi id of target device */
  int lun;
  int scsi_type;		/* TYPE_... defined in scsi/scsi.h */
  short h_cmd_per_lun;		/* host (adapter) maximum commands per lun */
  short d_queue_depth;		/* device (or adapter) maximum queue length */
  int unused1;			/* probably find a good use, set 0 for now */
  int unused2;			/* ditto */
}
My_sg_scsi_id;

typedef struct my_scsi_idlun
{
  int mux4;
  int host_unique_id;
}
My_scsi_idlun;

static const char *devices[] = {
  "/dev/sda", "/dev/sdb", "/dev/sdc", "/dev/sdd", "/dev/sde", "/dev/sdf",
  "/dev/sdg", "/dev/sdh", "/dev/sdi", "/dev/sdj", "/dev/sdk", "/dev/sdl",
  "/dev/sdm", "/dev/sdn", "/dev/sdo", "/dev/sdp", "/dev/sdq", "/dev/sdr",
  "/dev/sds", "/dev/sdt", "/dev/sdu", "/dev/sdv", "/dev/sdw", "/dev/sdx",
  "/dev/sdy", "/dev/sdz", "/dev/sdaa", "/dev/sdab", "/dev/sdac", "/dev/sdad",
  "/dev/scd0", "/dev/scd1", "/dev/scd2", "/dev/scd3", "/dev/scd4",
  "/dev/scd5",
  "/dev/scd6", "/dev/scd7", "/dev/scd8", "/dev/scd9", "/dev/scd10",
  "/dev/scd11", "/dev/sr0", "/dev/sr1", "/dev/sr2", "/dev/sr3", "/dev/sr4",
  "/dev/sr5",
  "/dev/sr6", "/dev/sr7", "/dev/sr8", "/dev/sr9", "/dev/sr10", "/dev/sr11",
  "/dev/cdrom", "/dev/cdrom0", "/dev/cdrom1", "/dev/cdrom2",
  "/dev/cdwriter", "/dev/cdwriter0", "/dev/cdwriter1", "/dev/cdwriter2",
  "/dev/dvd", "/dev/dvd0", "/dev/dvd1", "/dev/dvd2",
  "/dev/st0", "/dev/st1", "/dev/st2", "/dev/st3", "/dev/st4", "/dev/st5",
  "/dev/nst0", "/dev/nst1", "/dev/nst2", "/dev/nst3", "/dev/nst4",
  "/dev/nst5",
  "/dev/nosst0", "/dev/nosst1", "/dev/nosst2", "/dev/nosst3", "/dev/nosst4",
  "/dev/tape", "/dev/tape0", "/dev/tape1", "/dev/tape2", "/dev/tape3",
  "/dev/tape4",
  NULL
};

static map < string, string > sg_map;

static string scsi_handle(unsigned int host,
			  int channel = -1,
			  int id = -1,
			  int lun = -1)
{
  char buffer[10];
  string result = "SCSI:";

  snprintf(buffer, sizeof(buffer), "%02d", host);
  result += string(buffer);

  if (channel < 0)
    return result;

  snprintf(buffer, sizeof(buffer), "%02d", channel);
  result += string(":") + string(buffer);

  if (id < 0)
    return result;

  snprintf(buffer, sizeof(buffer), "%02d", id);
  result += string(":") + string(buffer);

  if (lun < 0)
    return result;

  snprintf(buffer, sizeof(buffer), "%02d", lun);
  result += string(":") + string(buffer);

  return result;
}

static const char *scsi_type(int type)
{
  switch (type)
  {
  case 0:
    return "Disk";
  case 1:
    return "Tape";
  case 3:
    return "Processor";
  case 4:
    return "Write-Once Read-Only Memory";
  case 5:
    return "CD-ROM";
  case 6:
    return "Scanner";
  case 7:
    return "Magneto-optical Disk";
  case 8:
    return "Medium Changer";
  case 0xd:
    return "Enclosure";
  default:
    return "";
  }
}

static int sg_err_category(int scsi_status,
			   int host_status,
			   int driver_status,
			   const unsigned char *sense_buffer,
			   int sb_len)
{
  scsi_status &= 0x7e;
  if ((0 == scsi_status) && (0 == host_status) && (0 == driver_status))
    return SG_ERR_CAT_CLEAN;
  if ((SCSI_CHECK_CONDITION == scsi_status) ||
      (SCSI_COMMAND_TERMINATED == scsi_status) ||
      (SG_ERR_DRIVER_SENSE == (0xf & driver_status)))
  {
    if (sense_buffer && (sb_len > 2))
    {
      int sense_key;
      unsigned char asc;

      if (sense_buffer[0] & 0x2)
      {
	sense_key = sense_buffer[1] & 0xf;
	asc = sense_buffer[2];
      }
      else
      {
	sense_key = sense_buffer[2] & 0xf;
	asc = (sb_len > 12) ? sense_buffer[12] : 0;
      }

      if (RECOVERED_ERROR == sense_key)
	return SG_ERR_CAT_RECOVERED;
      else if (UNIT_ATTENTION == sense_key)
      {
	if (0x28 == asc)
	  return SG_ERR_CAT_MEDIA_CHANGED;
	if (0x29 == asc)
	  return SG_ERR_CAT_RESET;
      }
    }
    return SG_ERR_CAT_SENSE;
  }
  if (0 != host_status)
  {
    if ((SG_ERR_DID_NO_CONNECT == host_status) ||
	(SG_ERR_DID_BUS_BUSY == host_status) ||
	(SG_ERR_DID_TIME_OUT == host_status))
      return SG_ERR_CAT_TIMEOUT;
  }
  if (0 != driver_status)
  {
    if (SG_ERR_DRIVER_TIMEOUT == driver_status)
      return SG_ERR_CAT_TIMEOUT;
  }
  return SG_ERR_CAT_OTHER;
}

static bool do_modesense(int sg_fd,
			 int page,
			 int page_code,
			 void *resp,
			 int mx_resp_len)
{
  int res;
  unsigned char senseCmdBlk[SENSE_CMD_LEN] =
    { SENSE_CMD_CODE, 0, 0, 0, 0, 0 };
  unsigned char sense_b[SENSE_BUFF_LEN];
  sg_io_hdr_t io_hdr;

  page &= 0x3f;
  page_code &= 3;

  senseCmdBlk[2] = (unsigned char) ((page_code << 6) | page);
  senseCmdBlk[4] = (unsigned char) 0xff;
  memset(&io_hdr, 0, sizeof(io_hdr));
  memset(sense_b, 0, sizeof(sense_b));
  io_hdr.interface_id = 'S';
  io_hdr.cmd_len = sizeof(senseCmdBlk);
  io_hdr.mx_sb_len = sizeof(sense_b);
  io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
  io_hdr.dxfer_len = mx_resp_len;
  io_hdr.dxferp = resp;
  io_hdr.cmdp = senseCmdBlk;
  io_hdr.sbp = sense_b;
  io_hdr.timeout = 20000;	/* 20 seconds */

  if (ioctl(sg_fd, SG_IO, &io_hdr) < 0)
    return false;

  res =
    sg_err_category(io_hdr.status, io_hdr.host_status, io_hdr.driver_status,
		    io_hdr.sbp, io_hdr.sb_len_wr);
  switch (res)
  {
  case SG_ERR_CAT_CLEAN:
  case SG_ERR_CAT_RECOVERED:
    return true;
  default:
    return false;
  }

  return true;
}

static bool do_inq(int sg_fd,
		   int cmddt,
		   int evpd,
		   unsigned int pg_op,
		   void *resp,
		   int mx_resp_len,
		   int noisy)
{
  int res;
  unsigned char inqCmdBlk[INQ_CMD_LEN] = { INQ_CMD_CODE, 0, 0, 0, 0, 0 };
  unsigned char sense_b[SENSE_BUFF_LEN];
  sg_io_hdr_t io_hdr;

  if (cmddt)
    inqCmdBlk[1] |= 2;
  if (evpd)
    inqCmdBlk[1] |= 1;
  inqCmdBlk[2] = (unsigned char) pg_op;
  inqCmdBlk[4] = (unsigned char) mx_resp_len;
  memset(&io_hdr, 0, sizeof(io_hdr));
  memset(sense_b, 0, sizeof(sense_b));
  io_hdr.interface_id = 'S';
  io_hdr.cmd_len = sizeof(inqCmdBlk);
  io_hdr.mx_sb_len = sizeof(sense_b);
  io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
  io_hdr.dxfer_len = mx_resp_len;
  io_hdr.dxferp = resp;
  io_hdr.cmdp = inqCmdBlk;
  io_hdr.sbp = sense_b;
  io_hdr.timeout = 20000;	/* 20 seconds */

  if (ioctl(sg_fd, SG_IO, &io_hdr) < 0)
    return false;

  res =
    sg_err_category(io_hdr.status, io_hdr.host_status, io_hdr.driver_status,
		    io_hdr.sbp, io_hdr.sb_len_wr);
  switch (res)
  {
  case SG_ERR_CAT_CLEAN:
  case SG_ERR_CAT_RECOVERED:
    return true;
  default:
    return false;
  }

  return true;
}

static unsigned long decode_3_bytes(void *ptr)
{
  unsigned char *p = (unsigned char *) ptr;

  return (p[0] << 16) + (p[1] << 8) + p[2];
}

static u_int16_t decode_word(void *ptr)
{
  unsigned char *p = (unsigned char *) ptr;

  return (p[0] << 8) + p[1];
}

static bool do_inquiry(int sg_fd,
		       hwNode & node)
{
  char ebuff[EBUFF_SZ];
  char rsp_buff[MX_ALLOC_LEN + 1];
  char version[5];
  int k;
  unsigned char len;

  if ((ioctl(sg_fd, SG_GET_VERSION_NUM, &k) < 0) || (k < 30000))
    return false;

  memset(rsp_buff, 0, sizeof(rsp_buff));
  if (!do_inq(sg_fd, 0, 0, 0, rsp_buff, 36, 1))
    return false;

  len = (unsigned char) rsp_buff[4] + 5;

  if ((len > 36) && (len < 256))
  {
    memset(rsp_buff, 0, sizeof(rsp_buff));
    if (!do_inq(sg_fd, 0, 0, 0, rsp_buff, len, 1))
      return false;
  }

  if (len != ((unsigned char) rsp_buff[4] + 5))
    return false;		// twin INQUIRYs yield different lengths

  unsigned ansiversion = rsp_buff[2] & 0x7;

  if (rsp_buff[1] & 0x80)
    node.addCapability("removable");

  node.setVendor(string(rsp_buff + 8, 8));
  if (len > 16)
    node.setProduct(string(rsp_buff + 16, 16));
  if (len > 32)
    node.setVersion(string(rsp_buff + 32, 4));

  snprintf(version, sizeof(version), "%d", ansiversion);
  if (ansiversion)
    node.setConfig("ansiversion", version);

  memset(rsp_buff, 0, sizeof(rsp_buff));
  if (do_inq(sg_fd, 0, 1, 0x80, rsp_buff, MX_ALLOC_LEN, 0))
  {
    len = rsp_buff[3];
    if ((len > 0) && (rsp_buff[0] > ' '))
      node.setSerial(string(rsp_buff + 4, len));
  }

  memset(rsp_buff, 0, sizeof(rsp_buff));
  if (do_modesense(sg_fd, 0x3F, 0, rsp_buff, sizeof(rsp_buff)))
  {
    unsigned long long sectsize = 0;
    unsigned long long heads = 0;
    unsigned long long cyl = 0;
    unsigned long long sectors = 0;
    unsigned long rpm = 0;
    u_int8_t *end = (u_int8_t *) rsp_buff + (u_int8_t) rsp_buff[0];
    u_int8_t *p = NULL;

    if (rsp_buff[3] == 8)
      sectsize = decode_3_bytes(rsp_buff + 9);

    p = (u_int8_t *) & rsp_buff[4];
    p += (u_int8_t) rsp_buff[3];
    while (p < end)
    {
      u_int8_t page = *p & 0x3F;

      if (page == 3)
      {
	sectors = decode_word(p + 10);
	sectsize = decode_word(p + 12);
      }
      if (page == 4)
      {
	cyl = decode_3_bytes(p + 2);
	rpm = decode_word(p + 20);
	heads = p[5];
      }

      p += p[1] + 2;
    }

    node.setCapacity(heads * cyl * sectors * sectsize);

    if (rpm / 10000 == 1)
      node.addCapability("10000rpm");
    else
    {
      if (rpm / 7200 == 1)
	node.addCapability("7200rpm");
      else
      {
	if (rpm / 5400 == 1)
	  node.addCapability("5400rpm");
      }
    }
  }

  return true;
}

static void scan_devices()
{
  int fd = -1;
  int i = 0;
  My_scsi_idlun m_idlun;

  for (i = 0; devices[i] != NULL; i++)
  {
    fd = open(devices[i], O_RDONLY | O_NONBLOCK);
    if (fd >= 0)
    {
      int bus = -1;
      if (ioctl(fd, SCSI_IOCTL_GET_BUS_NUMBER, &bus) >= 0)
      {
	memset(&m_idlun, 0, sizeof(m_idlun));
	if (ioctl(fd, SCSI_IOCTL_GET_IDLUN, &m_idlun) >= 0)
	{
	  sg_map[scsi_handle(bus, (m_idlun.mux4 >> 16) & 0xff,
			     m_idlun.mux4 & 0xff,
			     (m_idlun.mux4 >> 8) & 0xff)] =
	    string(devices[i]);
	}
      }
      close(fd);
    }
  }
}

static void find_logicalname(hwNode & n)
{
  map < string, string >::iterator i = sg_map.begin();

  for (i = sg_map.begin(); i != sg_map.end(); i++)
  {
    if (i->first == n.getHandle())
    {
      n.setLogicalName(i->second);
      n.claim();
      return;
    }
  }
}

static string host_logicalname(int i)
{
  char host[20];

  snprintf(host, sizeof(host), "scsi%d", i);

  return string(host);
}

static bool scan_sg(int sg,
		    hwNode & n)
{
  char buffer[20];
  int fd = -1;
  My_sg_scsi_id m_id;
  char slot_name[16];
  string host = "";
  hwNode *parent = NULL;
  hwNode *channel = NULL;
  int emulated = 0;

  snprintf(buffer, sizeof(buffer), SG_X, sg);

  fd = open(buffer, OPEN_FLAG | O_NONBLOCK);
  if (fd < 0)
    return false;

  memset(&m_id, 0, sizeof(m_id));
  if (ioctl(fd, SG_GET_SCSI_ID, &m_id) < 0)
  {
    close(fd);
    return true;		// we failed to get info but still hope we can continue
  }

  host = host_logicalname(m_id.host_no);

  memset(slot_name, 0, sizeof(slot_name));
  if (ioctl(fd, SCSI_IOCTL_GET_PCI, slot_name) >= 0)
  {
    string parent_handle = string("PCI:") + string(slot_name);

    parent = n.findChildByHandle(parent_handle);
  }

  if (!parent)
    parent = n.findChildByLogicalName(host);

  if (!parent)
    parent = n.addChild(hwNode("scsi", hw::bus));

  if (!parent)
  {
    close(fd);
    return true;
  }

  parent->setLogicalName(host);
  parent->claim();

  emulated = 0;
  ioctl(fd, SG_EMULATED_HOST, &emulated);

  if (emulated)
  {
    parent->addCapability("emulated");
  }

  channel =
    parent->findChildByHandle(scsi_handle(m_id.host_no, m_id.channel));
  if (!channel)
    channel = parent->addChild(hwNode("channel", hw::storage));

  if (!channel)
  {
    close(fd);
    return true;
  }

  snprintf(buffer, sizeof(buffer), "Channel %d", m_id.channel);
  channel->setDescription(buffer);
  channel->setHandle(scsi_handle(m_id.host_no, m_id.channel));
  channel->claim();

  hwNode device = hwNode("generic");

  switch (m_id.scsi_type)
  {
  case 0:
    device = hwNode("disk", hw::storage);
    break;
  case 1:
    device = hwNode("tape", hw::storage);
    break;
  case 3:
    device = hwNode("processor", hw::processor);
    break;
  case 4:
  case 5:
    device = hwNode("cdrom", hw::storage);
    break;
  case 6:
    device = hwNode("scanner", hw::generic);
    break;
  case 7:
    device = hwNode("magnetooptical", hw::storage);
    break;
  case 8:
    device = hwNode("changer", hw::generic);
    break;
  case 0xd:
    device = hwNode("enclosure", hw::generic);
    break;
  }

  device.setDescription(scsi_type(m_id.scsi_type));
  device.setHandle(scsi_handle(m_id.host_no,
			       m_id.channel, m_id.scsi_id, m_id.lun));
  find_logicalname(device);
  do_inquiry(fd, device);
  if ((m_id.scsi_type == 4) || (m_id.scsi_type == 5))
    scan_cdrom(device);
  if ((m_id.scsi_type == 0) || (m_id.scsi_type == 7))
    scan_disk(device);

  channel->addChild(device);

  close(fd);

  return true;
}

static int selectdir(const struct dirent *d)
{
  struct stat buf;

  if (d->d_name[0] == '.')
    return 0;

  if (lstat(d->d_name, &buf) != 0)
    return 0;

  return S_ISDIR(buf.st_mode);
}

static bool scan_hosts(hwNode & node)
{
  struct dirent **namelist = NULL;
  int n;
  vector < string > host_strs;

  if (!pushd("/proc/scsi"))
    return false;
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();
  if ((n < 0) || !namelist)
    return false;

  pushd("/proc/scsi");
  for (int i = 0; i < n; i++)
  {
    struct dirent **filelist = NULL;
    int m = 0;

    pushd(namelist[i]->d_name);
    m = scandir(".", &filelist, NULL, alphasort);
    popd();

    if (m >= 0)
    {
      for (int j = 0; j < m; j++)
      {
	char *end = NULL;
	int number = -1;

	number = strtol(filelist[j]->d_name, &end, 0);

	if ((number >= 0) && (end != filelist[j]->d_name))
	{
	  hwNode *controller =
	    node.findChildByLogicalName(host_logicalname(number));

	  if (controller)
	    controller->setConfig(string("driver"),
				  string(namelist[i]->d_name));
	}
	free(filelist[j]);
      }
      free(filelist);
    }
    free(namelist[i]);
  }
  free(namelist);
  popd();

  if (!loadfile("/proc/scsi/sg/host_strs", host_strs))
    return false;

  for (int i = 0; i < host_strs.size(); i++)
  {
    hwNode *host = node.findChildByLogicalName(host_logicalname(i));

    if (host)
    {
      if ((host->getProduct() == "") && (host->getDescription() == ""))
	host->setDescription(host_strs[i]);
    }
  }

  return true;
}

bool scan_scsi(hwNode & n)
{
  int i = 0;

  scan_devices();

  while (scan_sg(i, n))
    i++;

  scan_hosts(n);

  return false;
}

static char *id = "@(#) $Id: scsi.cc,v 1.28 2003/03/08 18:27:18 ezix Exp $";
