#!/bin/bash
#bins/bin-common.txt
#bins/bin-win.txt
name_common=$(cat bins/bin-common.txt);
name_msi=$(cat bins/bin-win.txt);
echo "URL:$name_common$name_msi";
#curl refs/tags/bins/$name_7z -o $name_7z;
mkdir tmp
pushd tmp
echo "curl -LOJ $name_common$name_msi;"
curl -LOJ $name_common$name_msi;
# name_7z_full=$(find . -type f -name $name_7z -print0 | xargs -0 realpath)
name_msi_full=$(find . -name "$name_msi" | xargs readlink -f)
echo "name_msi_full=$name_msi_full" >> $GITHUB_ENV
echo "list: pushd"
ls -l
msiexec /i $name_msi_full
popd
echo "list: popd"
ls -l
