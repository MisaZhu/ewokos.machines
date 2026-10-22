#!/bin/sh
# Copy a file to/from the Q8B without throttling. Optional LIMIT="-l 200"
# still allows an explicit bandwidth cap for a particular test.
ASKPASS=${ASKPASS:-$HOME/.dsh/q8b_askpass.sh}
export SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=none
exec scp -O ${LIMIT-} -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null "$@"
