<?PHP

// $Id$
//
// This gets called from within the header functions.  It establishes or
// connects to a PHP session, and then connects to Citadel if necessary.
//

function establish_citadel_session() {

	global $session, $clientsocket;

	if (strcmp('4.3.0', phpversion()) > 0) {
		die("This program requires PHP 4.3.0 or newer.");
	}


	session_start();

	if ($_SESSION["ctdlsession"]) {
		$session = $_SESSION["ctdlsession"];
	}
	else {
		$session = "CtdlSession." . time() . rand(1000,9999) ;
		$_SESSION["ctdlsession"] = $session;
	}

	// See if there's a Citadel connection proxy open for this session.
	// The name of the socket is identical to the name of the
	// session, and it's found in the /tmp directory.

	$sockname = "/tmp/" . $session . ".socket" ;

	$clientsocket = fsockopen($sockname, 0, $errno, $errstr, 5);
	if (!$clientsocket) {
		// It ain't there, dude.  Open up the proxy. (C version)
		//$cmd = "./sessionproxy " . $sockname ;
		//exec($cmd);

		// It ain't there, dude.  Open up the proxy.  (PHP version)
		$cmd = "./sessionproxy.php " . $sockname .
			" </dev/null >/dev/null 2>&1 " .
			" 3>&1 4>&1 5>&1 6>&1 7>&1 8>&1 & " ;
		exec($cmd);
		sleep(2);

		// Ok, now try again.
		$clientsocket = fsockopen($sockname, 0, $errno, $errstr, 5);

		// Try to log the user back in and go back to the correct room.
		if ($clientsocket) {

			ctdl_iden();	// Identify client

			if ($_SESSION["username"]) {
				login_existing_user(
					$_SESSION["username"],
					$_SESSION["password"]
				);
			}

			if ($_SESSION["room"]) {
				ctdl_goto($_SESSION["room"]);
			}
			else {
				ctdl_goto("_BASEROOM_");
			}
		}
	}

	if ($clientsocket) {
		if (!$_SESSION["serv_humannode"]) {
			ctdl_get_serv_info();
		}
	}
	else {
		echo "ERROR: no Citadel socket!<BR>\n";
		flush();
	}

	// If the user is trying to call up any page other than
	// login.php logout.php do_login.php,
	// and the session is not logged in, redirect to login.php
	//
	if ($_SESSION["logged_in"] != 1) {
		$filename = basename(getenv('SCRIPT_NAME'));
		if (	(strcmp($filename, "login.php"))
		   &&	(strcmp($filename, "logout.php"))
		   &&	(strcmp($filename, "do_login.php"))
		) {
			header("Location: login.php");
			exit(0);
		}
	}

	
}


//
// Clear out both our Citadel session and our PHP session.  We're done.
//
function ctdl_end_session() {
	global $clientsocket, $session;

	// Tell the Citadel server to terminate our connection.
	// (The extra newlines force it to see that the Citadel session
	// ended, and the proxy will quit.)
	//
	fwrite($clientsocket, "QUIT\n\n\n\n\n\n\n\n\n\n\n");
	$response = fgets($clientsocket, 4096);		// IGnore response
	fclose($clientsocket);
	unset($clientsocket);

	// Now clear our PHP session.
	$_SESSION = array();
	session_write_close();
}

?>
