#ifndef CHROMEDRIVE_INPUT_WAIT_H
#define CHROMEDRIVE_INPUT_WAIT_H

/* Wait for an eventfd wakeup without polling the input queue. Returns 1 when
 * the descriptor was drained, 0 on timeout, and -1 on an unrecoverable error.
 * EINTR is retried against the original monotonic deadline. */
int cd_input_wait(int fd,int timeout_ms);

#endif
