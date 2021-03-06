/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <diskboot.h>
#include <string.h>
#include <nand_bsp.h>
#include <malloc.h>
#include <vsprintf.h>
#include <bootdisk_interface.h>


static char *transfer_buffer    = (char*)0x41000000;
static char *malloc_base        = (char*)0x43000000; /* till 0x46ffffff */
char        *masstorage_buffer  = (char*)0x48000000;
static char *log_addr           = (char*)0x49000000;
// start of code is at 0x4a000000

static struct bootdisk_cmd_header cur_cmd;
static unsigned cur_cmd_bytes_written;

static int board_exit;
static void (*exitaddr_jumpto)(void) = (void(*)(void))0xffff0020; /* FEL address by default */

void dolog(const char *fmt, ...)
{
    static unsigned logno;

    if( log_addr <= (char*)0x49800000 ) {
        va_list args;

        sprintf(log_addr, "%-5u ", ++logno);
        log_addr += strlen(log_addr);
        va_start(args, fmt);
        vsprintf(log_addr, fmt, args);
        va_end(args);
        log_addr += strlen(log_addr);
    }
}

void diskboot_reset_handler(void)
{
    dolog("in reset handler\n");
    cur_cmd.cmd = BCMD_NONE;
}

static void send_resp_st(const void *data, int datasize, int status)
{
    struct bootdisk_resp_header resp;

    if( datasize < 0 )
        datasize = strlen(data);
    resp.magic = 0x1234;
    resp.status = status;
    resp.datasize = datasize;
    usbdcomm_diskboot_tx(&resp, sizeof(resp));
    if( datasize > 0 )
        usbdcomm_diskboot_tx(data, datasize);
}

static void send_resp_fail(void)
{
    send_resp_st(NULL, 0, BRST_FAIL);
}

static void send_resp_OK(const void *data, int datasize)
{
    send_resp_st(data, datasize, BRST_OK);
}

void send_resp_msg(const char *fmt, ...)
{
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsprintf(transfer_buffer, fmt, args);
    va_end(args);
    send_resp_st(transfer_buffer, len, BRST_MSG);
}

static void dispatch_cmd(uint16_t cmd, uint32_t param1, uint64_t start,
        uint32_t count, const char *data, unsigned datasize)
{
    int res;

    dolog("dispatch_cmd: command=%c, param1=%d,"
           " start=%lld (0x%llx), count=%d, datasize=%d\n",
           cmd, param1, start, start, count, datasize);
    switch(cmd) {
    case BCMD_FLASHMEM_PARAMS:
        {
            struct flashmem_properties props;
            FMT_GetFlashMemProperties(&props);
            send_resp_OK(&props, sizeof(struct flashmem_properties));
        }
        break;
    case BCMD_BOARD_EXIT:
        switch( param1 ) {
        case BE_GO_FEL:
            send_resp_OK("going to FEL...", -1);
            board_exit = 1;
            exitaddr_jumpto = (void(*)(void))0xffff0020;
            break;
        case BE_BOARD_RESET:
            send_resp_OK("board reset...", -1);
            board_exit = 1;
            exitaddr_jumpto = reset_board;
            break;
        }
        break;
    case BCMD_PING:
        send_resp_OK(NULL, 0);
        break;
    case BCMD_GETLOG:
        send_resp_OK((void*)0x49000000, log_addr - (char*)0x49000000);
        if( param1 != 0 )
            log_addr = (char*)0x49000000;
        break;
    case BCMD_SLEEPTEST:
        dolog("sleep time test: delay=%d\n", param1);
        sdelay(param1);
        send_resp_OK(NULL, 0);
        break;
    case BCMD_DISKREAD:
        dolog("rx_handler: read param1=%d, first=0x%llx, count=0x%x\n",
                param1, start, count);
        switch(param1) {
        case FMAREA_BOOT0:
            res = PHY_Boot0Read(start, count, transfer_buffer);
            break;
        case FMAREA_BOOT1:
            res = PHY_Boot1Read(start, count, transfer_buffer);
            break;
        default:    // FMAREA_DISK
            res = NAND_LogicRead(start, count, transfer_buffer);
        }
        if( res == 0 ) {
            send_resp_OK(transfer_buffer, count << 9);
            dolog("rx_handler read OK\n");
        }else{
            dolog("rx_handler read error: %d\n", res);
            send_resp_fail();
        }
        break;
    case BCMD_DISKWRITE:
        dolog("rx_handler: write param1=%d, first=0x%llx, size=0x%x\n",
                param1, start, datasize);
        switch(param1) {
        case FMAREA_BOOT0:
            res = PHY_Boot0Write(start, datasize/512, transfer_buffer);
            break;
        case FMAREA_BOOT1:
            res = PHY_Boot1Write(start, datasize/512, transfer_buffer);
            break;
        default:    // FMAREA_DISK
            res = LML_Write(start, datasize/512, transfer_buffer);
            if( res == 0 ) {
                dolog("rx_handler: LML_Write OK, flush cache\n");
                res = LML_FlushPageCache();
            }
        }
        if( res == 0 ) {
            send_resp_OK(NULL, 0);
            dolog("rx_handler write OK\n");
        }else{
            dolog("rx_handler write error: %d\n", res);
            send_resp_fail();
        }
        break;
    case BCMD_MEMREAD:
        send_resp_OK((const char*)(unsigned)start, count);
        break;
    case BCMD_MEMWRITE:
        memcpy((char*)(unsigned)start, transfer_buffer, datasize);
        send_resp_OK(NULL, 0);
        break;
    case BCMD_MEMEXEC:
        asm("blx %[jumpaddr]" : : [jumpaddr] "r" ((unsigned)start));
        send_resp_OK(NULL, 0);
        break;
    case BCMD_MEMJUMPTO:
        board_exit = 1;
        exitaddr_jumpto = (void(*)(void))(unsigned)start;
        send_resp_OK(NULL, 0);
        break;
    case BCMD_MOUNT:
        if( masstorage_mount(start, count, param1) != 0 ) {
            send_resp_fail();
        }else{
            send_resp_OK(NULL, 0);
        }
        break;
    case BCMD_GETMOUNT_STATUS:
        {
            unsigned firstSector, sectorCount;
            int read_write;
            struct diskmount_status stat;
            if( masstorage_getstatus(&firstSector, &sectorCount, &read_write)) {
                stat.mountMode = read_write ? MOUNTM_RW : MOUNTM_RO;
                stat.firstSector = firstSector;
                stat.sectorCount = sectorCount;
            }else{
                stat.mountMode = MOUNTM_NOMOUNT;
                stat.firstSector = 0;
                stat.sectorCount = 0;
            }
            send_resp_OK(&stat, sizeof(stat));
        }
        break;
    default:
        dolog("unknown command %c\n", cmd);
        send_resp_fail();
        break;
    }
}

int diskboot_rx_handler(const char *buffer, unsigned buffer_size)
{
    if( cur_cmd.cmd == BCMD_NONE ) {
        if( buffer_size != sizeof(cur_cmd) ) {
            dolog("rx_handler FATAL: wrong data size on input=%d\n",
                    buffer_size);
            return 0;
        }
        memcpy(&cur_cmd, buffer, sizeof(cur_cmd));
        if( cur_cmd.magic != 0x1234 ) {
            dolog("rx_handler FATAL: wrong magic on cmd=%x\n", cur_cmd.magic);
            return 0;
        }
        if( cur_cmd.datasize > 0x1000000 ) {    /* max 16 MB */
            dolog("rx_handler FATAL: too big data size on cmd=0x%x\n",
                    cur_cmd.datasize);
            return 0;
        }
        cur_cmd_bytes_written = 0;
    }else{
        int to_write = cur_cmd.datasize - cur_cmd_bytes_written;
        if( buffer_size <= to_write )
            to_write = buffer_size;
        else
            dolog("rx_handler: ignore extra %d bytes on input\n",
                    buffer_size - to_write);
        memcpy(transfer_buffer + cur_cmd_bytes_written, buffer, to_write);
        cur_cmd_bytes_written += to_write;
    }
    if( cur_cmd_bytes_written == cur_cmd.datasize ) {
        dispatch_cmd(cur_cmd.cmd, cur_cmd.param1, cur_cmd.start,
                cur_cmd.count, transfer_buffer, cur_cmd.datasize);
        cur_cmd.cmd = BCMD_NONE;
    }
    return 0;
}

void main_loop(void)
{
    clock_init();
    malloc_init(malloc_base, 0x4000000); // 64 MB
    NAND_Init();
    sdelay(0x100);
    if (0 == usbdcomm_init()) {
        while (1) {
            int poll_status = usbdcomm_poll();

            if (USBDCOMM_ERROR == poll_status) {
                /* Error */
                break;
            } else if (USBDCOMM_DISCONNECT == poll_status) {
                /* beak, cleanup and re-init */
                break;
            }
            if( board_exit )
                break;
        }
    }
    NAND_Exit();
    sdelay(0x200);
    usbdcomm_shutdown();
    clock_restore();
    exitaddr_jumpto();
}

