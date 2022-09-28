<h1>WebDash Client</h1>

<h2>How to use</h2>
<h3>Examples:</h3>
<ul>
  <li><code>webdash build</code></li>
  <li><code>webdash $#.build // to use definitions from /definitions.json</code></li>
  <li><code>webdash -register // to register the current directory to the server</code></li>
  <li><code>webdash -list // to list the available commands from the current directory</code></li>
  <li><code>webdash -list-config // to list <it>all</it> registered configs</code></li>
</ul>

<h3>How to design webdash.config.json</h3>
<pre><code>{
    "commands": [
      {
          "name": "required-identifier" // required.
          "action": "&lt;executable_name&gt; &lt;arguments&gt;"
          "actions": [
              "&lt;executable_name1&gt; &lt;arguments1&gt;"
              ...
              "&lt;executable_nameN&gt; &lt;argumentsN&gt;"
          ],
          "frequency": "daily",
          "when": "new-day",
          "wdir": "$.thisDir()",    
          "notify-dashboard": true // adds entry to notifications log output when run.
      }
    ]
}</code></pre>

<h3>Log output: <code>/app-temporary/logging/_webdash-client</code></h3>
