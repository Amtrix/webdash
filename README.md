<h1>WebDash: Another Multi-purpose Build Tool</h1>

Note: Current setup only validated for Ubuntu 18.04 LTS.
<h2>What it does?</h2>
A plugin based system to etend one's development tools. Simplifies a mutltitude of tasks.
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

This is your new development environment :).

<h3>/definitions.json</h3>
Stores global environment information. Example config follows.

<pre><code>{
    "myworld": {
        "rootDir": "this"
    },
    "env": {
        "MYWORLD": "$.rootDir()"
    },
    "path-add": [
        "$.rootDir()/app-persistent/bin"
    ],
    "pull-projects": [
        {
            "source": "https://github.com/Amtrix/src-bin-report-build-state",
            "destination": "$.rootDir()/src/bin/report-build-state",
            "exec": ":all"
        }
    ]
}
</pre></code>

<ul>
    <li><code>myworld.rootDir = this</code> - used by webdash to identify the root directory. Only a single definitions.json with such entry should exist.</li>
    <li><code>env.VAR</code> - adds environment variable VAR.</li>
    <li><code>path-add</code> - adds listed paths to PATH.</li>
    <li><code>pull-projects</code> - pulls listed projects when ./data/setup-webdash.sh is called.</li>
    <li><code>pull-projects.ENTRY.exec</code> - calls the specified entry within the project's webdash.config.json after the cloning.</li>
</ul>

<h3>WebDash Client</h3>
A client application that is able to parse user-created webdash.config.json files.
<h3>WebDash Server</h3>
A service running in the background that performs scheduled execution.
