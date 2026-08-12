#!/usr/bin/env bash

set -euo pipefail

temporary_directory=$(mktemp -d)
trap 'rm -rf -- "$temporary_directory"' EXIT HUP INT TERM

for repository in core extra; do
    mkdir -p "$temporary_directory/$repository"

    curl -fsSL "https://geo.mirror.pkgbuild.com/$repository/os/x86_64/$repository.db" | zstd -dc | tar -xf - -C "$temporary_directory/$repository"
done

requested=$(printf '%s\n' "$@")

awk -v requested="$requested" '
function clean_dependency(name) {
    sub(/[<=>].*$/, "", name)
    return name
}

function repository_of(path, parts, count) {
    count = split(path, parts, "/")
    return parts[count - 2]
}

function resolve(name, file, dependency, count, i) {
    sub(/^.*\//, "", name)
    name = clean_dependency(name)

    file = packages[name]

    if (file == "")
        file = providers[name]

    if (file == "") {
        print "error: unable to resolve dependency: " name > "/dev/stderr"
        failed = 1
        return
    }

    if (selected[file])
        return

    selected[file] = 1

    count = split(depends[file], dependency, "\n")

    for (i = 1; i <= count; i++) {
        if (dependency[i] != "")
            resolve(dependency[i])
    }
}

FNR == 1 {
    section = ""
}

/^%NAME%$/ {
    section = "NAME"
    next
}

/^%FILENAME%$/ {
    section = "FILENAME"
    next
}

/^%DEPENDS%$/ {
    section = "DEPENDS"
    next
}

/^%PROVIDES%$/ {
    section = "PROVIDES"
    next
}

/^%/ {
    section = ""
    next
}

NF {
    if (section == "NAME") {
        package_name[FILENAME] = $0
        packages[$0] = FILENAME
    } else if (section == "FILENAME") {
        package_filename[FILENAME] = $0
    } else if (section == "DEPENDS") {
        dependency = clean_dependency($0)

        if (depends[FILENAME] != "")
            depends[FILENAME] = depends[FILENAME] "\n" dependency
        else
            depends[FILENAME] = dependency
    } else if (section == "PROVIDES") {
        provider = clean_dependency($0)

        if (!(provider in providers))
            providers[provider] = FILENAME
    }
}

END {
    count = split(requested, requests, "\n")

    for (i = 1; i <= count; i++) {
        if (requests[i] != "")
            resolve(requests[i])
    }

    if (failed)
        exit 1

    for (file in selected)
        print repository_of(file) "/" package_name[file]
}
' "$temporary_directory"/*/*/desc | sort
