#include <bcm2712/mailbox.h>
#include <kernel/system.h>

void bcm2712_mailbox_read(mail_message_t *msg) {
    mail_status_t stat;
    uint8_t channel = msg->channel;

    // Make sure that the message is from the right channel
    do {
        // Make sure there is mail to recieve
        do {
            stat = *MAIL0_STATUS;
        } while (stat.empty);

        // Get the message
        *msg = *MAIL0_READ;
    } while (msg->channel != channel);
}

void bcm2712_mailbox_send(mail_message_t* msg) {
    mail_status_t stat;

    // Make sure you can send mail
    do {
        stat = *MAIL0_STATUS;
    } while (stat.full);

    // send the message
    *MAIL0_WRITE = *msg;
}

void bcm2712_mailbox_call(mail_message_t* msg) {
    flush_dcache();
    clear_cache((char*)msg, (char*)(msg + 1));
    bcm2712_mailbox_send(msg);
    bcm2712_mailbox_read(msg);
    clear_cache((char*)msg, (char*)(msg + 1));
}
