This file should contain:

  - Your name & UNI (or those of all group members for group assignments)
  - Homework assignment number
  - Description for each part

The description should indicate whether your solution for the part is working
or not. You may also want to include anything else you would like to
communicate to the grader, such as extra functionality you implemented or how
you tried to fix your non-working code.


part10 task3:
In this part I set a key to tell the parent process whether to print the stat or not, and this key can only be changed by the handler. Under my design, there is nothing special happen if I kill the child process. Because I didn't put key in the shared memory, child process can't changed the value of key in the parent process.
