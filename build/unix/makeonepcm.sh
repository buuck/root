#!/bin/sh
#
# Build a single large pcm for the entire basic set of ROOT libraries.
# Script takes as optional argument the source directory path.
#
# Copyright (c) 2013 Rene Brun and Fons Rademakers
# Author: Fons Rademakers, 19/2/2013

srcdir=.
if [ $# -eq 1 ]; then
   srcdir=$1
fi

rm -f include/allHeaders.h include/allHeaders.h.pch include/allLinkDef.h all.h cppflags.txt include/allLinkDef.h

# Need to ignore Qt because of its "#define emit" in qobjectdefs.h
for dict in `find ./*/ -name 'G__*.cxx' | grep -v '/qt' | grep -v /G__Cling.cxx`; do
    awk 'BEGIN{START=-1} /includePaths\[\] = {/, /0/ { if (START==-1) START=NR; else if ($0 != "0") { sub(/",/,"",$0); sub(/^"/,"-I",$0); print $0 } }' $dict >> cppflags.txt
    awk 'BEGIN{START=-1} /payloadCode =/, /^;$/ { if (START==-1) START=NR; else if ($1 != ";") { code=substr($0,2); sub(/\\n"/,"",code); print code } }' $dict >> all.h
    awk 'BEGIN{START=-1} /headers\[\] = {/, /0/ { if (START==-1) START=NR; else if ($0 != "0") { sub(/,/,"",$0); print "#include",$0 } }' $dict >> all.h
    dirname=`dirname $dict`
    dirname=`dirname $dirname`
    find $srcdir/$dirname/inc/ -name '*LinkDef*.h' | \
        sed -e 's|^|#include "|' -e 's|$|"|' >> alldefs.h
done

mv all.h include/allHeaders.h
mv alldefs.h include/allLinkDef.h

cxxflags="-D__CLING__ -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -Iinclude -Ietc -Ietc/cling `cat cppflags.txt | sort | uniq`"
#rm cppflags.txt

# generate one large pcm
rm -f allDict.* lib/allDict_rdict.pc*
touch etc/allDict.cxx.h
core/utils/src/rootcling_tmp -1 -f etc/allDict.cxx -c $cxxflags -I$srcdir include/allHeaders.h include/allLinkDef.h
res=$?
if [ $res -eq 0 ] ; then
  mv etc/allDict_rdict.pch etc/allDict.cxx.pch
  res=$?

  # actually we won't need the allDict.[h,cxx] files
  #rm -f allDict.*
fi

exit $res
