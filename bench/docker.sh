# Sourced by the scripts of bench/: finds out how to run docker.
#
# choose_docker sets and exports DOCKER, the command that the scripts run docker
# with. A DOCKER that the caller has set is kept. Otherwise it is `docker` if the
# user can talk to the daemon, and `sudo docker` if the daemon says "permission
# denied", the password being asked for once, here, and not in the middle of a run.

choose_docker() {
    if [ -n "${DOCKER:-}" ]; then
        export DOCKER
        return 0
    fi
    if ! command -v docker > /dev/null 2>&1; then
        echo "$0: docker is not installed" >&2
        exit 2
    fi
    local problem
    if problem=$(docker version --format '{{.Server.Version}}' 2>&1 > /dev/null); then
        export DOCKER=docker
        return 0
    fi
    if ! grep -qi "permission denied" <<< "$problem"; then
        echo "$0: docker does not answer: $problem" >&2
        exit 2
    fi
    if ! command -v sudo > /dev/null 2>&1; then
        echo "$0: the user cannot use docker: add them to the group docker, or install sudo" >&2
        exit 2
    fi
    if ! sudo -n true 2> /dev/null; then
        echo "== docker needs sudo: the password is asked for once" >&2
        sudo -v || exit 2
    fi
    if ! problem=$(sudo -n docker version --format '{{.Server.Version}}' 2>&1 > /dev/null); then
        echo "$0: docker does not answer with sudo either: $problem" >&2
        exit 2
    fi
    # sudo forgets the password after a while, and a matrix runs for hours: it is
    # renewed until this script is gone.
    local parent=$$
    (while kill -0 "$parent" 2> /dev/null; do sudo -n true 2> /dev/null; sleep 50; done) > /dev/null 2>&1 &
    export DOCKER="sudo docker"
}
