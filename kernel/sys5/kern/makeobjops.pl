#!/usr/bin/perl
#
# Copyright (c) 1992, 1993
#        The Regents of the University of California.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. All advertising materials mentioning features or use of this software
#    must display the following acknowledgement:
#        This product includes software developed by the University of
#        California, Berkeley and its contributors.
# 4. Neither the name of the University nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
# From @(#)vnode_if.sh        8.1 (Berkeley) 6/10/93
# From @(#)makedevops.sh 1.1 1998/06/14 13:53:12 dfr Exp $
# From @(#)makedevops.sh ?.? 1998/10/05
# From src/sys/kern/makedevops.pl,v 1.12 1999/11/22 14:40:04 n_hibma Exp
#
# $FreeBSD: src/sys/kern/makeobjops.pl,v 1.1 2000/04/08 14:17:10 dfr Exp $

#
# Script to produce kobj front-end sugar.
#

$debug = 0;
$cfile = 0;          # by default do not produce any file type
$hfile = 0;

$keepcurrentdir = 1;

$line_width = 80;

# Process the command line
#
while ( $arg = shift @ARGV ) {
   if ( $arg eq '-c' ) {
      warn "Producing .c output files"
         if $debug;
      $cfile = 1;
   } elsif ( $arg eq '-h' ) {
      warn "Producing .h output files"
         if $debug;
      $hfile = 1;
   } elsif ( $arg eq '-ch' || $arg eq '-hc' ) {
      warn "Producing .c and .h output files"
         if $debug;
      $cfile = 1;
      $hfile = 1;
   } elsif ( $arg eq '-d' ) {
      $debug = 1;
   } elsif ( $arg eq '-p' ) {
      warn "Will produce files in original not in current directory"
         if $debug;
      $keepcurrentdir = 0;
   } elsif ( $arg eq '-l' ) {
      if ( $line_width = shift @ARGV and $line_width > 0 ) {
         warn "Line width set to $line_width"
            if $debug;
      } else {
         die "Please specify a valid line width after -l";
      }
   } elsif ( $arg =~ m/\.m$/ ) {
      warn "Filename: $arg"
         if $debug;
      push @filenames, $arg;
   } else {
      warn "$arg ignored"
         if $debug;
   }
}


# Validate the command line parameters
#
die "usage: $0 [-d] [-p] [-l <nr>] [-c|-h] srcfile
where -c   produce only .c files
      -h   produce only .h files
      -p   use the path component in the source file for destination dir
      -l   set line width for output files [80]
      -d   switch on debugging
"
        unless ($cfile or $hfile)
           and $#filenames != -1;

# FIXME should be able to do this more easily
#
$tmpdir = $ENV{'TMPDIR'};           # environment variables
$tmpdir = $ENV{'TMP'}
   if !$tmpdir;
$tmpdir = $ENV{'TEMP'}
   if !$tmpdir;
$tmpdir = '/tmp'                    # look for a physical directory
   if !$tmpdir and -d '/tmp';
$tmpdir = '/usr/tmp'
   if !$tmpdir and -d '/usr/tmp';
$tmpdir = '/var/tmp'
   if !$tmpdir and -d '/var/tmp';
$tmpdir = '.'                       # give up and use current dir
   if !$tmpdir;

foreach $src ( @filenames ) {
   # Names of the created files
   $ctmpname = "$tmpdir/ctmp.$$";
   $htmpname = "$tmpdir/htmp.$$";

   ($name, $path, $suffix) = &fileparse($src, '.m');
   $path = '.'
      if $keepcurrentdir;
   $cfilename="$path/$name.c";
   $hfilename="$path/$name.h";

   warn "Processing from $src to $cfilename / $hfilename via $ctmpname / $htmpname"
      if $debug;

   die "Could not open $src, $!"
      if !open SRC, "$src";
   die "Could not open $ctmpname, $!"
      if $cfile and !open CFILE, ">$ctmpname";
   die "Could not open $htmpname, $!"
      if $hfile and !open HFILE, ">$htmpname";

   if ($cfile) {
      # Produce the header of the C file
      #
      print CFILE "/*\n";
      print CFILE " * This file is produced automatically.\n";
      print CFILE " * Do not modify anything in here by hand.\n";
      print CFILE " *\n";
      print CFILE " * Created from source file\n";
      print CFILE " *   $src\n";
      print CFILE " * with\n";
      print CFILE " *   $0\n";
      print CFILE " *\n";
      print CFILE " * See the source file for legal information\n";
      print CFILE " */\n";
      print CFILE "\n";
      print CFILE "#include <sys/param.h>\n";
      print CFILE "#include <sys/queue.h>\n";
      print CFILE "#include <sys/kernel.h>\n";
      print CFILE "#include <sys/kobj.h>\n";
   }

   if ($hfile) {
      # Produce the header of the H file
      #
      print HFILE "/*\n";
      print HFILE " * This file is produced automatically.\n";
      print HFILE " * Do not modify anything in here by hand.\n";
      print HFILE " *\n";
      print HFILE " * Created from source file\n";
      print HFILE " *   $src\n";
      print HFILE " * with\n";
      print HFILE " *   $0\n";
      print HFILE " *\n";
      print HFILE " * See the source file for legal information\n";
      print HFILE " */\n";
      print HFILE "\n";
   }

   %methods = ();    # clear list of methods
   @mnames = ();
   @defaultmethods = ();
   $lineno = 0;
   $error = 0;       # to signal clean up and gerror setting

   LINE:
   while ( $line = <SRC> ) {
       $lineno++;
       
       # take special notice of include directives.
       #
       if ( $line =~ m/^#\s*include\s+(["<])([^">]+)([">]).*/i ) {
	    warn "Included file: $1$2" . ($1 eq '<'? '>':'"')
            if $debug;
	    print CFILE "#include $1$2" . ($1 eq '<'? '>':'"') . "\n"
            if $cfile;
       }
       
       $line =~ s/#.*//;                # remove comments
	 $line =~ s/^\s+//;               # remove leading ...
       $line =~ s/\s+$//;               # remove trailing whitespace
       
       if ( $line =~ m/^$/ ) {          # skip empty lines
	   # nop
	   
       } elsif ( $line =~ m/^INTERFACE\s*([^\s;]*)(\s*;?)/i ) {
	   $intname = $1;
	   $semicolon = $2;
	   unless ( $intname =~ m/^[a-z_][a-z0-9_]*$/ ) {
	       warn $line
		 if $debug;
	       warn "$src:$lineno: Invalid interface name '$intname', use [a-z_][a-z0-9_]*";
	       $error = 1;
	       last LINE;
	   }
	   
	   warn "$src:$lineno: semicolon missing at end of line, no problem"
	     if $semicolon !~ s/;$//;
	   
	   warn "Interface $intname"
	     if $debug;
	   
	   print HFILE '#ifndef _'.$intname."_if_h_\n"
	     if $hfile;
	   print HFILE '#define _'.$intname."_if_h_\n\n"
	     if $hfile;
	   print CFILE '#include "'.$intname.'_if.h"'."\n\n"
	     if $cfile;
       } elsif ( $line =~ m/^CODE\s*{$/i ) {
	   $code = "";
	   $line = <SRC>;
	   $line =~ m/^(\s*)/;
	   $indent = $1;           # find the indent used
	   while ( $line !~ m/^}/ ) {
	   $line =~ s/^$indent//g; # remove the indent
	   $code .= $line;
	   $line = <SRC>;
	   $lineno++
       }
		 if ($cfile) {
		     print CFILE "\n".$code."\n";
		 }
	    } elsif ( $line =~ m/^HEADER\s*{$/i ) {
		$header = "";
		$line = <SRC>;
		$line =~ m/^(\s*)/;
		$indent = $1;              # find the indent used
		while ( $line !~ m/^}/ ) {
		$line =~ s/^$indent//g; # remove the indent
		$header .= $line;
		$line = <SRC>;
		$lineno++
	    }
		      if ($hfile) {
			  print HFILE $header;
		      }
		 } elsif ( $line =~ m/^(STATIC|)METHOD/i ) {
		     # Get the return type function name and delete that from
		     # the line. What is left is the possibly first function argument
		     # if it is on the same line.
		     #
		     if ( !$intname ) {
			 warn "$src:$lineno: No interface name defined";
			 $error = 1;
			 last LINE;
		     }
		     $line =~ s/^(STATIC|)METHOD\s+([^\{]+?)\s*\{\s*//i;
		     $static = $1;                                                    
		     @ret = split m/\s+/, $2;
		     $name = pop @ret;          # last element is name of method
		     $ret = join(" ", @ret);    # return type
		     
		     warn "Method: name=$name return type=$ret"
		       if $debug;
		     
		     if ( !$name or !$ret ) {
			 warn $line
			   if $debug;
			 warn "$src:$lineno: Invalid method specification";
			 $error = 1;
			 last LINE;
		     }
		     
		     unless ( $name =~ m/^[a-z_][a-z_0-9]*$/ ) {
			 warn $line
			   if $debug;
			 warn "$src:$lineno: Invalid method name '$name', use [a-z_][a-z0-9_]*";
			 $error = 1;
			 last LINE;
		     }
		     
		     if ( defined($methods{$name}) ) {
			 warn "$src:$lineno: Duplicate method name";
			 $error = 1;
			 last LINE;
		     }
		     
		     $methods{$name} = $name;
		     push @mnames, $name;
		     
		     while ( $line !~ m/}/ and $line .= <SRC> ) {
		     $lineno++
		 }
       
       $default = "";
       if ( $line !~ s/};?(.*)// ) { # remove first '}' and trailing garbage
       # The '}' was not there (the rest is optional), so complain
       warn "$src:$lineno: Premature end of file";
       $error = 1;
       last LINE;
   }
   $extra = $1;
   if ( $extra =~ /\s*DEFAULT\s*([a-zA-Z_][a-zA-Z_0-9]*)\s*;/ ) {
       $default = $1;
   } else {
       warn "$src:$lineno: Ignored '$1'"  # warn about garbage at end of line
	 if $debug and $1;
   }
   
   # Create a list of variables without the types prepended
   #
   $line =~ s/^\s+//;            # remove leading ...
   $line =~ s/\s+$//;            # ... and trailing whitespace
   $line =~ s/\s+/ /g;           # remove double spaces
   
   @arguments = split m/\s*;\s*/, $line;
   @varnames = ();               # list of varnames
   foreach $argument (@arguments) {
       next                       # skip argument if argument is empty
	 if !$argument;

       @ar = split m/[*\s]+/, $argument;
       if ( $#ar == 0 ) {         # only 1 word in argument?
	   warn "$src:$lineno: no type for '$argument'";
	   $error = 1;
	   last LINE;
       }
       
       push @varnames, $ar[-1];   # last element is name of variable
   };
   
   warn 'Arguments: ' . join(', ', @arguments) . "\n"
     . 'Varnames: ' . join(', ', @varnames)
       if $debug;
   
   $mname = $intname.'_'.$name;  # method name
   $umname = uc($mname);         # uppercase method name
   
   $arguments = join(", ", @arguments);
   $firstvar = $varnames[0];
   $varnames = join(", ", @varnames);
   
   $default = "0" if $default eq "";
   push @defaultmethods, $default;
   
   if ($hfile) {
       # the method description 
       print HFILE "extern struct kobjop_desc $mname\_desc;\n";
       # the method typedef
       print HFILE &format_line("typedef $ret $mname\_t($arguments);",
				$line_width, ', ',
				',',' ' x length("typedef $ret $mname\_t("))
	 . "\n";
   }
   
   if ($cfile) {
       # Print out the method desc
       print CFILE "struct kobjop_desc $mname\_desc = {\n";
       print CFILE "\t0, (kobjop_t) $default\n";
       print CFILE "};\n\n";
   }
   
   if ($hfile) {
       # Print out the method itself
       if (0) {                 # haven't chosen the format yet
	   print HFILE "static __inline $ret $umname($varnames)\n";
	   print HFILE "\t".join(";\n\t", @arguments).";\n";
       } else {
	   print HFILE &format_line("static __inline $ret $umname($arguments)",
				    $line_width, ', ',
				    ',', ' ' x length("$ret $umname(")) . "\n";
       }
       print HFILE "{\n";
       print HFILE "\tkobjop_t _m;\n";
       if ( $static ) {
	   print HFILE "\tKOBJOPLOOKUP($firstvar->ops,$mname);\n";
       } else {
	   print HFILE "\tKOBJOPLOOKUP(((kobj_t)$firstvar)->ops,$mname);\n";
       }
       print HFILE "\t";
       if ($ret ne 'void') {
	   print HFILE "return ";
       }
       print HFILE "(($mname\_t *) _m)($varnames);\n";
       print HFILE "}\n\n";
   }
} else {
    warn $line
      if $debug;
    warn "$src:$lineno: Invalid line encountered";
    $error = 1;
    last LINE;
}
} # end LINE

# print the final '#endif' in the header file
#
print HFILE "#endif /* _".$intname."_if_h_ */\n"
  if $hfile;

close SRC;
close CFILE
  if $cfile;
close HFILE
  if $hfile;

if ( !$error ) {
    if ($cfile) {
	($rc = system("mv $ctmpname $cfilename"))
	  and warn "mv $ctmpname $cfilename failed, $rc";
    }
    
    if ($hfile) {
	($rc = system("mv $htmpname $hfilename"))
	  and warn "mv $htmpname $hfilename failed, $rc";
    }
} else {
    warn 'Output skipped';
    ($rc = system("rm -f $htmpname $ctmpname"))
      and warn "rm -f $htmpname $ctmpname failed, $rc";
    $gerror = 1;
}
}

exit $gerror;


sub format_line {
    my ($line, $maxlength, $break, $new_end, $new_start) = @_;
    my $rline = "";
    
    while ( length($line) > $maxlength
	    and ($i = rindex $line, $break, $maxlength-length($new_end)) != -1 ) {
	$rline .= substr($line, 0, $i) . $new_end . "\n";
	$line = $new_start . substr($line, $i+length($break));
    }
    
    return $rline . $line;
}

# This routine is a crude replacement for one in File::Basename. We
# cannot use any library code because it fouls up the Perl bootstrap
# when we update a perl version. MarkM

sub fileparse {
    my ($filename, @suffix) = @_;
    my ($dir, $name, $type, $i);
    
    $type = '';
    foreach $i (@suffix) {
	if ($filename =~ m|$i$|) {
	    $filename =~ s|$i$||;
	    $type = $i;
	}
    }
    if ($filename =~ m|/|) {
	$filename =~ m|([^/]*)$|;
	$name = $1;
	$dir = $filename;
	$dir =~ s|$name$||;
    }
    else {
	$dir = '';
	$name = $filename;
    }
    ($name, $dir, $type);
}

sub write_interface {
    $mcount = $#mnames + 1;
}
