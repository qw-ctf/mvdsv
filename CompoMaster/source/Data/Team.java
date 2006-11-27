/**

$Id: Team.java,v 1.2 2006/11/27 15:15:48 vvd0 Exp $

**/

package Data;

import java.io.Serializable;

// Referenced classes of package Data:
//            RealPlayer, Player

public class Team extends RealPlayer
    implements Serializable
{

    public Team(String name)
    {
        super(name);
        memberName = new String[0];
    }

    public Team(String name, String password)
    {
        super(name, password);
        memberName = new String[0];
    }

    public Team(String name, String password, int rank, String shortname, String hp, String irc)
    {
        super(name, password);
        this.hp = hp;
        this.shortname = shortname;
        this.irc = irc;
        super.rank = rank;
        memberName = new String[0];
    }

    public void addMember(String name)
    {
        String temp[] = new String[memberName.length + 1];
        System.arraycopy(memberName, 0, temp, 0, memberName.length);
        temp[temp.length - 1] = name;
        memberName = temp;
    }

    public void setMember(int idx, String name)
    {
        memberName[idx] = name;
    }

    public void removeMember(int index)
    {
        if(index < 0 || index > memberName.length - 1)
            return;
        String temp[] = new String[memberName.length - 1];
        int di = 0;
        for(int si = 0; si < memberName.length; si++)
            if(si != index)
            {
                temp[di] = memberName[si];
                di++;
            }

        memberName = temp;
    }

    public String getMember(int i)
    {
        if(i < memberName.length)
            return memberName[i];
        else
            return null;
    }

    public int getMemberCount()
    {
        return memberName.length;
    }

    public String[] getTeamMemberArray()
    {
        return memberName;
    }

    public String getName()
    {
        return super.name;
    }

    public String getShortname()
    {
        return shortname;
    }

    public String toString()
    {
        return new String((new StringBuffer(super.name)).append(":").append(super.password).append(":").append(super.rank).append(":").append(shortname).append(":").append(hp).append(":").append(irc));
    }

    String memberName[];
    String hp;
    String irc;
    String shortname;
}