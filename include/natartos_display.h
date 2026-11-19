/* SD display handshake inside the existing 64-byte fb_shared header.
 * Legacy Doom/PC Stream headers do not have this magic and keep their protocol.
 * Lock files are persistent inodes: never unlink them, even during injection.
 */
#ifndef NATARTOS_DISPLAY_H
#define NATARTOS_DISPLAY_H

#define NAT_DISPLAY_MAGIC "NATSD01"
#define NAT_DISPLAY_MAGIC_BYTES 8
#define NAT_DISPLAY_MAGIC_OFFSET 8
#define NAT_DISPLAY_ACK_OFFSET 16
#define NAT_DISPLAY_ACK_READY 1u
#define NAT_DISPLAY_OWNER "fb_owner.lock"
#define NAT_DISPLAY_OVERLAY "fb_overlay.lock"
#define NAT_DISPLAY_WAIT_MS 5000

#endif
