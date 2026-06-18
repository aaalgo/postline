

def generate_shell_instruction ():
    # obtain environment variable POSTLINE_HOME
    # enumerate files in glob(POSTLINE_HOME/bin/shell/*)
    # for each one, document can be obtained by running the tool using subprocess.check_output with --doc

    # maintain a list of messages
    # for each tool,
    # append one with:
    #   From: ai
    #   To: shell
    #   Subject: tool_name --doc
    
    # with no content
    # and append one with
    #   From: shell
    #   To: ai
    #   Subject: tool_name --doc
    
    # With the document as content


    # then return the list
