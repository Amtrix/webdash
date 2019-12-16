<h1>WebDash: Another Multi-purpose Build Tool</h1>

Note: Current setup only validated for Ubuntu 18.04 LTS.
<h2>What it does?</h2>
A plugin based system to add new tools to the development. Simplifies the following.
<ul>
  <li>Automated script execution</li>
  <li>Dependency resolution</li>
  <li>Command aliases</li>
  <li>Output piping and analysis</li>
  <li>Notifier system</li>
  <li>Centralized logging</li>
  <li>Development environment setup and automation</li>
</ul>

<h2>How does it work?</h2>
Executing data/setup-webdash.sh creates the following directory structure:

<pre><code>app-persistent/
    bin/
    lib/
app-temporary/
    logging/
data/
    setup-webdash.sh
src/
    bin/
        _webdash-client/
        _webdash-server/
    lib/
        external/
            json/
            websocketpp/
        webdash-executer/
definitions.json
.gitignore
init.sh
</pre></code>

<h3>WebDash Client</h3>
A client application that is able to parse user-created webdash.config.json files.
<h3>WebDash Server</h3>
A service running in the background that performs scheduled execution.
