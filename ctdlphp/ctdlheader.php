<?PHP

// $Id$
//
// Header and footer code to be included on every page.  Not only does it
// contain some common markup, but it also calls some code glue that holds
// the session together.
//
// Copyright (c) 2003 by Art Cancro <ajc@uncensored.citadel.org>
// This program is released under the terms of the GNU General Public License.


// All of the back-end magic gets included from here.  The rest of the
// pages in the system then only have to include ctdlheader.php (since it
// is required) and they get the others automatically.
//
include "ctdlsession.php";
include "ctdlprotocol.php";
include "ctdlelements.php";

function bbs_page_header() {

	// Make sure we're connected to Citadel.  Do not remove this!!
	establish_citadel_session();

	echo <<<LITERAL
<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN">
<html>
<head>
	<meta http-equiv="Content-Type" content="text/html; charset=iso-8859-1">
	<meta name="Description" content="Citadel BBS">
	<meta name="Keywords" content="citadel,bbs">
	<meta name="MSSmartTagsPreventParsing" content="TRUE">
	<title>
LITERAL;

	if ($_SESSION["serv_humannode"]) {
		echo $_SESSION["serv_humannode"] ;
	}
	else {
		echo "BBS powered by Citadel" ;
	}

	echo <<<LITERAL
</title>
</head>

<body
	text="#000000"
	bgcolor="#FFFFFF"
	link="#0000FF"
	vlink="#990066"
	alink="#DD0000"
>


LITERAL;

	echo '<TABLE BORDER=0 WIDTH=100%>';
	echo '<TR>';
	echo '<TD>' . $_SESSION["serv_humannode"] . '</TD>' ;
	echo '<TD>' . $_SESSION["username"] . '</TD>' ;
	echo '<TD>' . $_SESSION["room"] . '</TD>' ;
	echo '<TD ALIGN=RIGHT><A HREF="logout.php">Log out</A></TD>' ;
	echo '</TR></TABLE><BR>';

	// Temporary menu
	if ($_SESSION["logged_in"]) {
		echo	'<a href="listrooms.php">' .
			'room list</A> ' .
			'<a href="readmsgs.php?mode=all&count=0">' .
			'Read all messages</a> ' .
			'<a href="readmsgs.php?mode=new&count=0">' .
			'Read new messages</a> ' .
			'<a href="display_enter.php">' .
			'Enter a message</a> ' .
			'<a href="who.php">' .
			'Who is online?</a> ' .
			'<A HREF="logout.php">' .
			'Log out</A><HR>' ;
	}

}


function bbs_page_footer() {
	echo "<HR>";
	echo "Powered by Citadel.  And a few cups of coffee.<BR>\n";
	echo "</BODY></HTML>\n";
}


?>
