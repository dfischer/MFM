##
# Begin: common.pl.inc

use Carp;
use File::Path qw(make_path);
use File::Spec;
use File::Temp qw(tempdir);
use IO::Uncompress::Unzip qw($UnzipError);
use Digest::SHA qw(sha512_hex sha512);

sub VERSION { "1.0" }
sub MFZRUN_HEADER { "MFZ(".VERSION.")\n" }
sub MFZ_PUBKEY_NAME { "MFZPUBKEY.DAT" }
sub MFZ_FILE_NAME { "MFZNAME.DAT" }
sub MFZ_SIG_NAME { "MFZSIG.DAT" }
sub MFZ_ZIP_NAME { "MFZ.ZIP" }

use constant CDM_FORMAT_MAGIC => "CDM";
use constant CDM_FORMAT_VERSION_MAJOR => "1";
use constant CDM_FORMAT_VERSION_MINOR => "0";

use constant CDM10_PACK_SIGNED_DATA_FORMAT =>
    ""            #   0
    ."a6"         #   0 +   6 =    6 magic + maj + min + \n
    ."C1"         #   6 +   1 =    7 bits in block size
    ."C1"         #   7 +   1 =    8 regnum
    ."N"          #   8 +   4 =   12 slotstamp
    ."N"          #  12 +   4 =   16 mapped file length
    ."a16"        #  16 +  16 =   32 label
    ."a64"        #  32 +  64 =   96 sha512 mapped file checksum
    ."(a8)100"    #  96 + 800 =  896 100*8 byte xsum map
    #  896 total length
    ;

use constant CDM10_PACK_FULL_FILE_FORMAT =>
    ""            #   0
    ."a896"       #   0 + 896 =  896 as from CDM10_PACK_SIGNED_DATA_FORMAT
    ."a128"       # 896 + 128 = 1024 RSA sig by regnum of bytes 0..895
    # 1024 total length
    ;

my $programName = $0;  # default
sub SetProgramName {
    $programName = shift;
}

sub IDie {
    my $msg = shift;
    print STDERR "\nInternal Error: $msg\n";
    confess "I suck";
}

my $UDIE_MSG;
sub SetUDieMsg {
    $UDIE_MSG = shift;
}

sub UDie {
    my $msg = shift;
    IDie("Unset UDie message") unless defined $UDIE_MSG;
    print STDERR "\nError: $msg\n";
    print STDERR $UDIE_MSG;
    exit(1);
}

sub NoVerb {
    UDie("Missing command");
}

my $KeyDir;

sub InitMFMSubDir {
    my $sub = shift;
    IDie "No sub?" unless defined $sub;
    IDie "No KD?" unless defined $KeyDir;

    my $dir = "$KeyDir/$sub"; # $KeyDir should be clean and $sub should be internal

    if (!-d $dir) {
        make_path($dir)       # So we shouldn't need untainting to do this
            or die "Couldn't mkdir $dir: $!";
    }
    return $dir;
}

sub GetPublicKeyDir {
    return InitMFMSubDir("public_keys");
}

sub GetPublicKeyFile {
    my $handle = shift;
    my $ehandle = EscapeHandle($handle);
    my $dir = GetPublicKeyDir();
    my $pub = "$dir/$ehandle.pub";

    return $pub;
}

sub JoinHandleToKey {
    my ($handle, $keydata) = @_;
    my $data ="[MFM-Handle:$handle]\n$keydata";
    return $data;
}

sub SplitHandleFromKey {
    my $data = shift;
    $data =~ s/^\[MFM-Handle:(:?[a-zA-Z][-., a-zA-Z0-9]{0,62})\]\r?\n//
        or return undef;
    my $handle = $1;
    return ($handle,$data);
}

sub ComputeFingerprintFromFullPublicKey {
    my $fullpubkey = shift;
    my $fingerprint = lc(sha512_hex($fullpubkey));
    $fingerprint =~ s/^(....)(...)(....).+$/$1-$2-$3/
        or IDie("you're a cow");
    return $fingerprint;
}

sub ComputeChecksumOfString {
    my $string = shift;
    my $fingerprint = lc(sha512_hex($string));
    $fingerprint =~ s/^(......)(..).+(..)(......)$/$1-$2$3-$4/
        or IDie("give me some milk or else go home");
    return $fingerprint;
}

sub ComputeChecksumPrefixOfString {
    my ($string,$len) = @_;
    IDie("something is happening here but you don't know what it is")
        unless defined $len && $len >= 0 && $len <= 64;
    my $checksum = sha512($string);
    return substr($checksum,0,$len);
}

sub ReadPublicKeyFile {
    my $handle = shift;
    my $file = GetPublicKeyFile($handle);
    my $data = ReadWholeFile($file);
    my ($pubhandle, $key) = SplitHandleFromKey($data);
    UDie("Bad format in public key file '$file' for '$handle'")
        unless defined $pubhandle;
    UDie("Non-matching handle in public key file '$file' ('$handle' vs '$pubhandle')")
        unless $pubhandle eq $handle;
    return ($key, ComputeFingerprintFromFullPublicKey($data));
}

sub CheckForPubKey {
    my $handle = shift;
    my $path = GetPublicKeyFile($handle);
    if (-r $path) {
        return ($path, ReadPublicKeyFile($handle));
    }
    return ($path);
}

sub GetConfigDir {
    my $cfgdir = InitMFMSubDir("config");
    chmod 0700, $cfgdir;
    return $cfgdir;
}

sub GetLegalHandle {
    my $handle = shift;
    if ($handle eq "-") {
        my $defaulthandle = GetDefaultHandle();
        if (!defined $defaulthandle) {
            print STDERR "ERROR: No default handle, so cannot use '-' as handle (try 'mfzmake help'?)\n";
            exit 1;
        }
        return $defaulthandle;
    }

    UntaintHandleIfLegal(\$handle)
        or UDie("Bad handle '$handle'");
    return $handle;
}

sub GetLegalRegnum {
    my $regnum = shift;
    UDie("Not a number '$regnum'")
        unless $regnum =~ /^(\d+)$/;
    my $num = $1;
    UDie("Illegal regnum $num")
        unless $regnum >= 0 && $regnum < (1<<16);
    my @regnumHandles = (
        "t2-keymaster-release-10"
        );
    my $handle = $regnumHandles[$num];
    UDie("Invalid regnum $num")
        unless defined $handle;
    return ($num, $handle);
}

sub GetDefaultHandleFile {
    my $dir = GetConfigDir();
    my $def = "$dir/defaultHandle";
    return $def;
}

sub GetDefaultHandle {
    my $file = GetDefaultHandleFile();
    if (-r $file) {
        my $handle = ReadWholeFile($file);
        return $handle if UntaintHandleIfLegal(\$handle);
    }
    return undef;
}

sub GetPrivateKeyDir {
    my $privdir = InitMFMSubDir("private_keys");
    chmod 0700, $privdir;
    return $privdir;
}

sub GetPrivateKeyFile {
    my $handle = shift;
    my $ehandle = EscapeHandle($handle);
    my $dir = GetPrivateKeyDir();
    my $pub = "$dir/$ehandle.priv";
    return $pub;
}

sub ReadPrivateKeyFile {
    my $handle = shift;
    my $file = GetPrivateKeyFile($handle);
    my $data = ReadWholeFile($file);
    my ($privhandle, $key) = SplitHandleFromKey($data);
    UDie("Bad format in private key file for '$handle'")
        unless defined $privhandle;
    UDie("Non-matching handle in private key file ('$handle' vs '$privhandle')")
        unless $privhandle eq $handle;
    return $key;
}

sub VersionExit {
    my $pname = shift;
    $pname = "" unless defined $pname;
    print "$pname-".VERSION."\n";
    exit(0);
}

sub GetKeyDir {
    IDie("No key dir?") unless defined $KeyDir;
    return $KeyDir;
}

sub KDGetVerb {
    my $mustExist = shift;
    my $verb = NextArg();
    NoVerb() if $mustExist && !defined $verb;
    my $kdir;
    if (defined($verb) && $verb eq "-kd") {
        $kdir = NextArg();
        UDie("Missing argument to '-kd' switch") 
            unless defined $kdir;
        $verb = NextArg();
        NoVerb() if $mustExist && !defined $verb;
    } else {
        $kdir = glob "~/.mfm";
    }

    # Let's avoid accidentally creating keydir 'help' or whatever..
    UDie("-kd argument ('$kdir') must begin with '/', './', or '../'")
        unless $kdir =~ m!^([.]{0,2}/.*)$!;
    $KeyDir = $1;

    if (-e $KeyDir) {
        UDie("'$KeyDir' exists but is not a directory")
            if ! -d $KeyDir;
    }
    return $verb;
}

sub NextArg {
    my $arg = shift @ARGV;
    return $arg;
}

sub LastArg {
    my $arg = NextArg();
    my @more = RestOfArgs();
    UDie("Too many arguments: ".join(" ",@more))
        if scalar(@more);
    return $arg;
}

sub RestOfArgs {
    my @args = @ARGV;
    @ARGV = ();
    return @args;
}

sub ReadableFileOrDie {
    my ($text, $path) = @_;
    UDie "No $text provided" unless defined $path;
    UDie "Non-existent or unreadable $text: '$path'"
        unless -r $path and -f $path;
    $path =~ /^(.+)$/
      or IDie("am i here all alone");
    return $1;
}

sub WritableFileOrDie {
    my ($text, $path) = @_;
    UDie "No $text provided" unless defined $path;
    UDie "Unwritable $text: '$path': $!" unless -w $path or !-e $path;
    $path =~ /^(.+)$/
      or IDie("hands you a bone");
    return $1;
}

sub ReadWholeFile {
    my $file = shift;
    open (my $fh, "<", $file) or IDie("Can't read '$file': $!");
    local $/ = undef;
    my $content = <$fh>;
    close $fh or IDie("Failed closing '$file': $!");
    return $content;
}

sub WriteWholeFile {
    my ($file, $content, $perm) = @_;
    open (my $fh, ">", $file) or UDie("Can't write '$file': $!");
    chmod $perm, $fh
        if defined $perm;
    print $fh $content;
    close $fh or IDie("Failed closing '$file': $!");
}

sub UntaintHandleIfLegal {
    my $ref = shift;
    return 0
        unless $$ref =~ /^\s*(:?[a-zA-Z][-., a-zA-Z0-9]{0,62})\s*$/;
    $$ref = $1;
    return 1;
}

sub EscapeHandle {
    my $handle = shift;
    chomp($handle);
    $handle =~ s/([^a-zA-Z0-9])/sprintf("%%%02x",ord($1))/ge;
    return $handle;
}

sub UnzipStreamToMemory {
    my ($u) = @_;
    my @paths;

    my $status;
    my $count = 0;
    for ($status = 1; $status > 0; $status = $u->nextStream(), ++$count) {
        my $header = $u->getHeaderInfo();
        my $stored_time = $header->{'Time'};
        $stored_time =~ /^(\d+)$/ or die "Bad stored time: '$stored_time'";
        $stored_time = $1;  # Untainted

        my $fullpath = $header->{Name};
        my (undef, $path, $name) = File::Spec->splitpath($fullpath);

        if ($name eq "" or $name =~ m!/$!) {
            last if $status < 0;
        } else {

            my $data = "";
            my $buff;
            while (($status = $u->read($buff)) > 0) {
                $data .= $buff;
            }
            if ($status == 0) {
                push @paths, [$path, $name, $stored_time, $data];
            }
        }
    }

    die "Error in processing: $!\n"
        if $status < 0 ;
    return @paths;
}

sub UnzipStream {
    my ($u, $dest) = @_;
    my @paths;

    $dest = "." unless defined $dest;

    my $status;
    my $count = 0;
    for ($status = 1; $status > 0; $status = $u->nextStream(), ++$count) {
        my $header = $u->getHeaderInfo();
        my $stored_time = $header->{'Time'};
        $stored_time =~ /^(\d+)$/ or die "Bad stored time: '$stored_time'";
        $stored_time = $1;  # Untainted

        my (undef, $path, $name) = File::Spec->splitpath($header->{Name});
        my $destdir = "$dest/$path";

        my $totouch;
        unless (-d $destdir) {
            make_path($destdir)
                or die "Couldn't mkdir $destdir: $!";
            $totouch = $destdir;
        }
        if ($name eq "" or $name =~ m!/$!) {
            last if $status < 0;
        } else {

            my $destfile = "$destdir$name";
            my $buff;
#            print STDERR "Writing $destfile\n";
            my $fh = IO::File->new($destfile, "w")
                or die "Couldn't write to $destfile: $!";
            my $length = 0;
            while (($status = $u->read($buff)) > 0) {
                 $length += length($buff);
#                print STDERR "Read ".length($buff)."\n";
                $fh->write($buff);
            }
            $fh->close();
            $totouch = $destfile;
            push @paths, [$destdir, $name, $stored_time, $length];
        }

        utime ($stored_time, $stored_time, $totouch)
            or die "Couldn't touch $totouch: $!";
    }

    die "Error in processing: $!\n"
        if $status < 0 ;
    return @paths;
}

# Returns:
# undef if $findName (in option $findPath) is not found,
# [$destdir, $name, $stored_time] if $pref is data from UnzipStream
# [$path, $name, $stored_time, $data] if $pref is data from UnzipStreamToMemory

sub FindName {
    my ($pref, $findName, $findPath) = @_;
    my @precs = @{$pref};
    for my $rec (@precs) {
        my @fields = @{$rec};
        my ($path, $name) = @fields;
        if ($name eq $findName) {
            if (!defined($findPath) || $path eq $findPath) {
                return @fields;
            }
        }
    }
    return undef;
}

sub SignStringRaw {
    my ($privkeyfile, $datatosign) = @_;

    my $keystring = ReadWholeFile( $privkeyfile );
    my $privatekey = Crypt::OpenSSL::RSA->new_private_key($keystring);
    $privatekey->use_pkcs1_padding();
    $privatekey->use_sha512_hash();
    my $signature = $privatekey->sign($datatosign);
    return $signature;
}

sub SignString {
    my ($privkeyfile, $datatosign) = @_;
    my $signature = SignStringRaw($privkeyfile, $datatosign);
    return encode_base64($signature, '');
}


# End: common.pl.inc
###
