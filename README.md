<h1>WebDash</h1>
Introduces a unique way of aliasing commands while working with bash. Adds some extra abilities:

<ul>
    <li>Invokes dependencies.</li>
    <li> A consistent environment for projects.</li>
    <li>Directory-relative references</li>
    <ul>
        <li>You can use <code>$.rootDir()/src/projectA</code> to reference <code>projectA</code> when building some <code>projectB</code>.
    </ul>
</ul>

<h2>Setup</h2>
<h3>Install Dependencies</h3>

<pre><code># Includes newer tools.
sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y
sudo apt-get update

sudo apt-get install git
sudo apt-get install cmake
sudo apt-get install make
sudo apt-get install libboost-all-dev

# Perform ONLY if your g++ version is below g++-9.
sudo apt-get install g++-9
sudo apt-get install gcc-9
sudo ln -sf g++-9 /usr/bin/g++
</pre></code>

<h3>Run the Setup Script</h3>
Call <code>./data/setup-webdash.sh</code> to initialize everything. It will use your <code>git</code> client to clone additionally required projects.

<br/>

<h2>Enforced Environment</h2>
A special directory hierarchy is enforced by WebDash.

&nbsp;<br>

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
        webdash-executor/
webdash-profile.json
.gitignore
webdash.terminal.init.sh
</pre></code>

This is your new development environment.

<h2>webdash-profile.json</h2>
Located in the top-level directory after setup is completed. Defines all global behavior.

&nbsp;<br>

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
            "exec": ":all",
            "register": true
        }
    ]
}
</pre></code>

<b>Note: Entry in pull-projects is listed as an example.</b>

Meaning of all fields:
<ul>
    <li><code>myworld.rootDir = this</code></li>
    <ul><li>Used by webdash to identify the root directory. Only a single definitions.json with such entry should exist.</li></ul>
    <li><code>env.VAR</code></li>
    <ul><li>adds environment variable VAR.</li></ul>
    <li><code>path-add</code></li>
    <ul><li>adds listed paths to PATH.</li></ul>
    <li><code>pull-projects</code></li>
    <ul><li>pulls listed projects when ./data/setup-webdash.sh is called.</li></ul>
    <li><code>pull-projects.ENTRY.exec</code></li>
    <ul><li>calls the specified entry within the project's webdash.config.json file after the cloning.</li></ul>
</ul>
