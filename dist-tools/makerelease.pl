#!/usr/bin/perl -w
#
# Make a fetchmail release.
# Dumps a release notice and diffs as a MIME multipart message 
# in RELEASE_NOTES
#

my $project = "fetchmail";
my $uploaddir = "branch_6.5";
my $website = "https://downloads.sourceforge.net/project/$project/$uploaddir/";
my $mailfrom = "<$project-devel\@lists.sourceforge.net> (Fetchmail Development Team)";
my $xzsufx =	'.tar.xz';

# ---------------------------------------------------------------------

use POSIX qw(strftime);
use Getopt::Long;
use strict vars;
use warnings;

# check environment
(-r "NEWS" and -r "fetchmail.c" and -r "configure.ac") or die "Please cd to the top-level source directory!";
die "Need GNU sort!" unless `sort --version | head -n1` =~ /GNU/;

# parse options
my $diffs = 0;
my $verbose = 0;
my $help = 0;
my $null = ">/dev/null";
my $errnull = "2>/dev/null";

sub usage($$) {
    my ($own, $rc) = @_;

    print STDERR "Usage: $_[0] [--verbose,-v] [--help,-h,-?]\n";
    exit($_[1]);
}

sub makerelnotes($$) {
    my ($infile, $outfile) = @_;
    open(F, "<$infile") or die "cannot read $infile: $!";
    open(G, ">$outfile") or die "cannot write to $outfile: $!";
    my $ctr = 0;
    while(<F>) {
	$ctr++ if /^fetchmail-/;
	print G if $ctr == 1;
    }
    close F or die "cannot read $infile: $!";
    close G or die "cannot write to $outfile: $!";
}

GetOptions("diffs|d" => \$diffs, "verbose|v" => \$verbose, "help|h|?" => \$help)
    or usage($0, 1);

usage($0, 0) if $help;

die "$0 does not yet work with --diffs - needs to be updated for Git first!" if $diffs;

if ($verbose) {
    $null = "";
}

my $tmp = $ENV{TMPDIR} || $ENV{TMP} || $ENV{TEMP} || "/tmp";

# extract version from source
my $version =`grep 'AC_INIT' configure.ac`;
$version =~ /AC_INIT\([^,]*,\[?([0-9.rcbetalphdv-]+)\]?\,.*\)/;
$version = $1;
die "cannot determine version" unless defined $1;
my $tag = "RELEASE_$version";
$tag =~ tr/./-/;

# extract existing tags
my @versions;
open(ID, "git tag | sort -t- -k1,1 -k2,2n -k3,3n |") || die "cannot run git tag: $!\naborting";
while (<ID>) {
	chomp;
	if (m{^(RELEASE_.*)$}) {
		unshift(@versions, $1);
	}
}
close ID || die "git tag   failed, aborting";

my $oldtag; my $oldver;
if ($versions[0] eq $tag) {
	$tag = $versions[0];
	$oldtag = $versions[1];
} else {
	$tag = '<workfile>';
	$oldtag = $versions[0];
}

my $pwd = `pwd`; chomp $pwd;

$ENV{PATH} .= ":$pwd/dist-tools";

print "Building $version release, tag $tag, previous tag $oldtag\n";

if (-d "autom4te.cache") {
	system("rm -rf autom4te.cache")
		and die "Failure in removing autom4te.cache";
}

printf "### autoreconf\n";

if (system("autoreconf -ifs" . ($verbose ? 'v' : ''))) {
	die("Failure in regenerating autoconf files\n");
}

print "### configure\n";

system("rm -rf autobuild") and die("Cannot rm -rf autobuild directory\n");

if (system("mkdir -p autobuild && cd autobuild " 
	. " && ../configure -C --silent --with-ssl")) { die("Configuration failure\n"); }

print "### Test-building the software...\n";
my $ncpu;
open(my $p, "-|", "nproc 2>/dev/null || gnproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_CONF 2>/dev/null || echo 2") or die "cannot find number of CPUs";
if (not defined ($ncpu = <$p>)) { die "cannot read number of CPUs"; }
close $p or die "could not find number of CPUs";

chomp $ncpu;

print "--- CPUs for make: $ncpu\n";

if ($ncpu < 1) { warn "ncpus unplausible, assuming 2"; $ncpu = 2; }

if (system("cd autobuild && make -j$ncpu " . ($verbose ? '' : '-s') . " check distcheck")) {
	die("Compilation failure\n");
}

open(REPORT, ">$tmp/$project.PREAMBLE.$$");

print REPORT <<EOF;
From: $mailfrom
Subject: The $version release of $project is available

The $version release of $project is now available at the usual locations,
including <$website>.

The source archive is available at:
<$website$project-$version$xzsufx/download>

Here are the release notes:

EOF

# Extract the current notes
open(NEWS, "NEWS");
while (<NEWS>) {
	if (/^$project/) {
		print REPORT $_;
		last;
	}
}
while (<NEWS>) {
	if (/^$project/) {
		last;
	}
	print REPORT $_;
}

$oldver = $oldtag;
$oldver =~ tr/-/./;
$oldver =~ s/^RELEASE_//;

if ($diffs) {
	print REPORT "Diffs from the previous ($oldver) release follow as a MIME attachment."
} else {
	print REPORT "By popular demand, diffs from the previous release have been omitted."
}

close(NEWS);

close(REPORT);

if ($diffs) {
	if ($tag eq '<workfile>') {
		system("svn diff -r$oldtag        $errnull >$tmp/$project.DIFFS.$$");
	} else {
		system("svn diff -r$oldtag -r$tag $errnull >$tmp/$project.DIFFS.$$");
	}
	print "Diff size:";
	system("wc <$tmp/$project.DIFFS.$$");

	system "metasend -b"
	." -D '$project-$tag announcement' -m 'text/plain' -e 7bit -f $tmp/$project.PREAMBLE.$$"
	." -n -D 'diff between $oldver and $version' -m 'text/plain' -e 7bit -f $tmp/$project.DIFFS.$$"
	." -o ANNOUNCE.EMAIL";
} else {
	system("mv", "$tmp/$project.PREAMBLE.$$", "ANNOUNCE.EMAIL");
}

#unlink("$tmp/$project.PREAMBLE.$$");
unlink("$tmp/$project.DIFFS.$$");

print "### Signing tarballs...\n";
system("cd autobuild && gpg -ba --sign $project-$version$xzsufx");

print "### Extracting release notes...\n";
makerelnotes('NEWS', 'autobuild/README.txt');

print "### Uploading\n";
print "=== local\n";

system("rsync -acvHP autobuild/$project-$version$xzsufx autobuild/$project-$version$xzsufx.asc autobuild/README.txt m-a\@frs.sourceforge.net:/home/frs/project/fetchmail/$uploaddir/");
# unlink 'autobuild/README' or die "cannot unlink autobuild/README: $!";

print "=== Done - please review final tasks\n";

system("cat RELEASE-INSTRUCTIONS");

# makerelease ends here
